#include "aseba.h"
#include <memory>

#include <QDebug>
#include <sstream>

std::vector<sint16> toAsebaVector(const QList<int>& values)
{
	std::vector<sint16> data;
	data.reserve(values.size());
	for (int i = 0; i < values.size(); ++i)
		data.push_back(values[i]);
	return data;
}

QList<int> fromAsebaVector(const std::vector<sint16>& values)
{
	QList<int> data;
	for (size_t i = 0; i < values.size(); ++i)
		data.push_back(values[i]);
	return data;
}

static const char* exceptionSource(Dashel::DashelException::Source source) {
	switch(source) {
	case Dashel::DashelException::SyncError: return "SyncError";
	case Dashel::DashelException::InvalidTarget: return "InvalidTarget";
	case Dashel::DashelException::InvalidOperation: return "InvalidOperation";
	case Dashel::DashelException::ConnectionLost: return "ConnectionLost";
	case Dashel::DashelException::IOError: return "IOError";
	case Dashel::DashelException::ConnectionFailed: return "ConnectionFailed";
	case Dashel::DashelException::EnumerationError: return "EnumerationError";
	case Dashel::DashelException::PreviousIncomingDataNotRead: return "PreviousIncomingDataNotRead";
	case Dashel::DashelException::Unknown: return "Unknown";
	}
	qFatal("undeclared dashel exception source %i", source);
}

void DashelHub::start(QString target) {
	try {
		auto closeStream = [this](Dashel::Stream* stream) { return this->closeStream(stream); };
		std::unique_ptr<Dashel::Stream, decltype(closeStream)> stream(Dashel::Hub::connect(target.toStdString()), closeStream);
		run();
	} catch(Dashel::DashelException& e) {
		const char* source(exceptionSource(e.source));
		const char* reason(e.what());
		qWarning("DashelException(%s, %s, %s, %p)", source, strerror(e.sysError), reason, e.stream);
		emit error(source, reason);
	}
}

AsebaClient::AsebaClient() {
	hub.moveToThread(&thread);

	QObject::connect(&hub, &DashelHub::connectionCreated, &hub, [this](Dashel::Stream* stream) {
		this->stream = stream;
		{
			Aseba::GetDescription message;
			send(&message);
		}
		{
			Aseba::ListNodes message;
			send(&message);
		}
	}, Qt::DirectConnection);

	QObject::connect(&hub, &DashelHub::incomingData, &hub, [this](Dashel::Stream* stream) {
		auto message(Aseba::Message::receive(stream));
		std::unique_ptr<Aseba::Message> ptr(message);
		Q_UNUSED(ptr);

		std::wostringstream dump;
		message->dump(dump);
		qDebug() << "received" << QString::fromStdWString(dump.str());

		switch (message->type) {
		case ASEBA_MESSAGE_NODE_PRESENT:
			if (manager.getDescription(message->source) == nullptr) {
				Aseba::GetNodeDescription response(message->source);
				send(&response);
			}
			break;
		case ASEBA_MESSAGE_DESCRIPTION:
		case ASEBA_MESSAGE_NAMED_VARIABLE_DESCRIPTION:
		case ASEBA_MESSAGE_LOCAL_EVENT_DESCRIPTION:
		case ASEBA_MESSAGE_NATIVE_FUNCTION_DESCRIPTION:
		case ASEBA_MESSAGE_DISCONNECTED:
			manager.processMessage(message);
			break;
		default: {
				Aseba::UserMessage* userMessage(dynamic_cast<Aseba::UserMessage*>(message));
				if (userMessage)
					emit this->userMessage(userMessage->type, fromAsebaVector(userMessage->data));
			}
		}

	}, Qt::DirectConnection);

	QObject::connect(&hub, &DashelHub::error, this, &AsebaClient::connectionError, Qt::QueuedConnection);

	QObject::connect(&manager, &AsebaDescriptionsManager::nodeDescriptionReceived, this, [this](unsigned nodeId) {
		auto description = manager.getDescription(nodeId);
		nodes.append(new AsebaNode(this, nodeId, description));
		emit this->nodesChanged();
	});

	thread.start();
}

AsebaClient::~AsebaClient() {
	hub.stop();
	thread.quit();
	thread.wait();
}

void AsebaClient::start(QString target) {
	QMetaObject::invokeMethod(&hub, "start", Qt::QueuedConnection, Q_ARG(QString, target));
}

void AsebaClient::send(Aseba::Message* message) {
	if (stream) {
		message->serialize(stream);
		stream->flush();
	}
}

AsebaNode::AsebaNode(AsebaClient* parent, unsigned nodeId, const Aseba::TargetDescription* description)
	: QObject(parent), nodeId(nodeId), description(*description) {
	unsigned dummy;
	variablesMap = description->getVariablesMap(dummy);
}

void AsebaNode::setVariable(QString name, QList<int> value) {
	uint16 start = variablesMap[name.toStdWString()].first;
	Aseba::SetVariables::VariablesVector variablesVector(value.begin(), value.end());
	Aseba::SetVariables message(nodeId, start, variablesVector);
	parent()->send(&message);
}

void AsebaNode::setProgram(QString source) {
	Aseba::Compiler compiler;
	compiler.setTargetDescription(&description);
	Aseba::CommonDefinitions commonDefinitions;
	commonDefinitions.events.push_back(Aseba::NamedValue(L"block", 1));
	commonDefinitions.events.push_back(Aseba::NamedValue(L"link", 2));
	compiler.setCommonDefinitions(&commonDefinitions);

	std::wistringstream input(source.toStdWString());
	Aseba::BytecodeVector bytecode;
	unsigned allocatedVariablesCount;
	Aseba::Error error;
	bool result = compiler.compile(input, bytecode, allocatedVariablesCount, error);

	if (!result) {
		qWarning() << QString::fromStdWString(error.toWString());
		qWarning() << source;
		return;
	}

	std::vector<Aseba::Message*> messages;
	Aseba::sendBytecode(messages, nodeId, std::vector<uint16>(bytecode.begin(), bytecode.end()));
	foreach (auto message, messages) {
		parent()->send(message);
		delete message;
	}

	Aseba::Run run(nodeId);
	parent()->send(&run);
}
