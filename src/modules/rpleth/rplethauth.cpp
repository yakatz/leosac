/**
 * \file rplethauth.cpp
 * \author Thibault Schueller <ryp.sqrt@gmail.com>
 * \brief rpleth compatibility module
 */

#include "rplethauth.hpp"

#include <sstream>
#include <iostream>

#include "network/unixsocket.hpp"
#include "rplethprotocol.hpp"
#include "tools/unixsyscall.hpp"
#include "tools/log.hpp"
#include "tools/unlock_guard.hpp"
#include "exception/moduleexception.hpp"

RplethAuth::RplethAuth(ICore& core, const std::string& name)
:   _core(core),
    _name(name),
    _isRunning(true),
    _serverSocket(nullptr),
    _port(0),
    greenLed_(nullptr),
    buzzer_(nullptr),
    _fdMax(0),
    _timeout(DefaultTimeoutMs)
{}

const std::string& RplethAuth::getName() const
{
    return (_name);
}

IModule::ModuleType RplethAuth::getType() const
{
    return (ModuleType::Auth);
}

void RplethAuth::serialize(ptree& node)
{
    node.put("port", _port);

    _isRunning = false;
    _networkThread.join();
}

void RplethAuth::deserialize(const ptree& node)
{
    _port = node.get<Rezzo::UnixSocket::Port>("port", Rezzo::ISocket::Port(DefaultPort));
    std::string green_led_device_name = node.get("greenLed", "");
    std::string buzzer_device_name = node.get("buzzer", "");

    if (!green_led_device_name.empty())
    {
        greenLed_ = dynamic_cast<Led*>(_core.getHWManager().getDevice(green_led_device_name));
    }
    if (!buzzer_device_name.empty())
    {
        buzzer_ = dynamic_cast<Led*>(_core.getHWManager().getDevice(buzzer_device_name));
    }


    _networkThread = std::thread([this] () { run(); } );
}

void RplethAuth::authenticate(const AuthRequest& ar)
{
    std::istringstream          iss(ar.getContent());
    CardId                      cid;
    unsigned int                byte;
    std::lock_guard<std::mutex> lg(_cardIdQueueMutex);

    while (!iss.eof())
    {
        iss >> std::hex >> byte;
        cid.push_back(static_cast<Byte>(byte));
        // drop the colon delimeter
        char trash;
        iss >> trash;
    }

    if (cid == CardId{0x40, 0x61, 0x81, 0x80})
    {
        play_test_card_melody();
    }

    // reset card ID
    if (cid == CardId{0x56, 0xbb, 0x28, 0xc5})
    {
        reset_application();
    }

    _cardIdQueue.push(cid);
    _core.getModuleProtocol().cmdAuthorize(ar.getId(), true);
}

void RplethAuth::run()
{
    std::size_t     readRet;
    int             selectRet;

    _serverSocket = new Rezzo::UnixSocket(Rezzo::ISocket::Protocol::TCP);
    _serverSocket->bind(_port);
    _serverSocket->listen();
    while (_isRunning)
    {
        buildSelectParams();
        if ((selectRet = ::select(_fdMax + 1, &_rSet, nullptr, nullptr, &_timeoutStruct)) == -1)
            throw (ModuleException(UnixSyscall::getErrorString("select", errno)));
        else if (!selectRet)
            handleCardIdQueue();
        else
        {
            for (auto it = _clients.begin(); it != _clients.end(); ++it)
            {
                if (FD_ISSET(it->socket->getHandle(), &_rSet))
                {
                    try {
                        readRet = it->socket->recv(_buffer, RingBufferSize);
                        it->buffer.write(_buffer, readRet);
                        handleClientMessage(*it);
                    }
                    catch (const ModuleException& e) {
                        it->socket->close();
                        delete it->socket;
                        _clients.erase(it);
                        LOG() << "Client disconnected";
                        break;
                    }
                }
            }
            if (FD_ISSET(_serverSocket->getHandle(), &_rSet))
            {
                _clients.push_back(Client(_serverSocket->accept()));
                LOG() << "Client connected";
            }
        }
    }
    for (auto& client : _clients)
    {
        client.socket->close();
        delete client.socket;
    }
    _serverSocket->close();
    delete _serverSocket;
}

void RplethAuth::buildSelectParams()
{
    FD_ZERO(&_rSet);
    FD_SET(_serverSocket->getHandle(), &_rSet);
    _fdMax = _serverSocket->getHandle();
    for (auto& client : _clients)
    {
        _fdMax = std::max(_fdMax, client.socket->getHandle());
        FD_SET(client.socket->getHandle(), &_rSet);
    }
    _timeoutStruct.tv_sec = _timeout / 1000;
    _timeoutStruct.tv_usec = (_timeout % 1000) * 1000;
}

void RplethAuth::handleClientMessage(RplethAuth::Client& client)
{
    RplethPacket packet(RplethPacket::Sender::Client);

    do
    {
        packet = RplethProtocol::decodeCommand(client.buffer);
        if (!packet.isGood)
            break;
        RplethPacket response = RplethProtocol::processClientPacket(this, packet);
        std::size_t size = RplethProtocol::encodeCommand(response, _buffer, RingBufferSize);
        client.socket->send(_buffer, size);
    }
    while (packet.isGood && client.buffer.toRead());
}

void RplethAuth::handleCardIdQueue()
{
    CardId                      cid;
    RplethPacket                packet(RplethPacket::Sender::Server);
    std::lock_guard<std::mutex> lg(_cardIdQueueMutex);
    std::size_t                 size;

    packet.status = RplethProtocol::Success;
    packet.type = RplethProtocol::HID;
    packet.command = RplethProtocol::Badge;
    while (!_cardIdQueue.empty())
    {
        cid = _cardIdQueue.front();
        _cardIdQueue.pop();
        {
            unlock_guard<std::mutex>    ulg(_cardIdQueueMutex);

            packet.dataLen = cid.size();
            packet.data = cid;
            size = RplethProtocol::encodeCommand(packet, _buffer, RingBufferSize);
            for (auto& client : _clients)
                client.socket->send(_buffer, size);
        }
    }
}

IDevice *RplethAuth::getBuzzer() const
{
    return buzzer_;
}

IDevice *RplethAuth::getGreenLed() const
{
    return greenLed_;
}

void RplethAuth::play_test_card_melody()
{
    LOG() << "Test card found.";
    std::thread t([this] {
        for (int x = 0; x < 5; ++x)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (greenLed_)
                greenLed_->turnOn();
            if (buzzer_)
                buzzer_->turnOn();
             std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (greenLed_)
                greenLed_->turnOff();
            if (buzzer_)
                buzzer_->turnOff();
        }
    });

    t.detach();

}

void RplethAuth::reset_application()
{
    _core.reset();
}
