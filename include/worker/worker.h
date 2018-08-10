#pragma once

#include <infra/infra.h>

#include <string>
#include <exception>

#include <proto/faasm.pb.h>
#include <Runtime/Runtime.h>

using namespace IR;
using namespace Runtime;

namespace worker {
    const std::string ENTRYPOINT_FUNC = "run";

    const int MAX_NAME_LENGTH = 20;

    // Input memory
    const int INPUT_START = 0;
    const int MAX_INPUT_BYTES = 1024 * 1024;

    // Output memory
    const int OUTPUT_START = INPUT_START + MAX_INPUT_BYTES;
    const int MAX_OUTPUT_BYTES = 1024 * 1024;

    // Chaining memory
    const int MAX_CHAINS = 100;
    const int CHAIN_NAMES_START = OUTPUT_START + MAX_OUTPUT_BYTES;
    const int MAX_CHAIN_NAME_BYTES = MAX_NAME_LENGTH * MAX_CHAINS;

    const int CHAIN_DATA_START = CHAIN_NAMES_START + MAX_CHAIN_NAME_BYTES;
    const int MAX_CHAIN_DATA_BYTES = MAX_INPUT_BYTES * MAX_CHAINS;

    /** Wrapper for wasm code */
    class WasmModule {
    public:
        WasmModule();

        /** Executes the function and stores the result */
        int execute(message::FunctionCall &call);

        /** Cleans up */
        void clean();

    private:
        ModuleInstance *moduleInstance;
        ValueTuple functionResults;
    };

    /** Worker wrapper */
    class Worker {
    public:
        Worker();

        void start();

        /** Called when one function wants to make call into another */
        static void chainFunction(U8 *userName,  U8 *funcName, U8* inputData, I32 inputLength) ;
    private:
        static infra::RedisClient redis;
    };

    /** Exceptions */
    class WasmException : public std::exception {
    };

    class InvalidResultException : public std::exception {
    };
}