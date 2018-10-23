#pragma once

#include <iostream>
#include <string>

#include <proto/faasm.pb.h>
#include <hiredis/hiredis.h>

namespace infra {
    /** Function utilities */
    std::string getFunctionStubDir();

    std::string getFunctionFile(const message::FunctionCall &call);

    std::string getFunctionObjectFile(const message::FunctionCall &call);

    std::vector<uint8_t> getFunctionObjectBytes(const message::FunctionCall &call);

    bool isValidFunction(const message::FunctionCall &call);

    /** Serialisation */
    std::vector<uint8_t> callToBytes(const message::FunctionCall &call);

    /** Redis client */
    class Redis {

    public:

        Redis();

        /** Returns the instance for the current thread */
        static Redis *getThreadConnection();

        void reconnect();

        std::vector<uint8_t> get(const std::string &key);

        void set(const std::string &key, const std::vector<uint8_t> &value);

        void setRange(const std::string &key, long offset, const std::vector<uint8_t> &value);

        std::vector<uint8_t> getRange(const std::string &key, long start, long end);

        void enqueue(const std::string &queueName, const std::vector<uint8_t> &value);

        std::vector<uint8_t> dequeue(const std::string &queueName);

        void flushAll();

        long listLength(const std::string &queueName);

        void callFunction(message::FunctionCall &call);

        message::FunctionCall nextFunctionCall();

        void setFunctionResult(message::FunctionCall &call, bool success);

        message::FunctionCall getFunctionResult(const message::FunctionCall &call);
    private:
        redisContext *context;
    };
}
