#include "math.h"
#include "syscalls.h"

#include <wasm/WasmModule.h>
#include <wavm/WAVMWasmModule.h>

#include <WAVM/Runtime/Intrinsics.h>
#include <WAVM/Runtime/Runtime.h>

#include <faabric/mpi/MpiContext.h>
#include <faabric/mpi/mpi.h>
#include <faabric/scheduler/ExecutorContext.h>
#include <faabric/scheduler/Scheduler.h>
#include <faabric/util/gids.h>
#include <faabric/util/logging.h>

using namespace faabric::mpi;
using namespace faabric::scheduler;
using namespace WAVM;

#define MPI_FUNC(str)                                                          \
    SPDLOG_TRACE("MPI-{} {}", executingContext.getRank(), str);

#define MPI_FUNC_ARGS(formatStr, ...)                                          \
    SPDLOG_TRACE("MPI-{} " formatStr, executingContext.getRank(), __VA_ARGS__);

namespace wasm {
static thread_local faabric::mpi::MpiContext executingContext;

bool isInPlace(U8 wasmPtr)
{
    return wasmPtr == FAABRIC_IN_PLACE;
}

MpiWorld& getExecutingWorld()
{
    MpiWorldRegistry& reg = getMpiWorldRegistry();
    return reg.getWorld(executingContext.getWorldId());
}

/**
 * Convenience wrapper around the MPI context for use in the syscalls in this
 * file.
 */
class ContextWrapper
{
  public:
    explicit ContextWrapper()
      : module(getExecutingWAVMModule())
      , memory(module->defaultMemory)
      , world(getExecutingWorld())
      , rank(executingContext.getRank())
    {}

    void checkMpiComm(I32 wasmPtr)
    {
        faabric_communicator_t* hostComm =
          &Runtime::memoryRef<faabric_communicator_t>(memory, wasmPtr);

        if (hostComm->id != FAABRIC_COMM_WORLD) {
            SPDLOG_ERROR("Unrecognised communicator type {}", hostComm->id);
            throw std::runtime_error("Unexpected comm type");
        }
    }

    faabric_datatype_t* getFaasmDataType(I32 wasmPtr)
    {
        faabric_datatype_t* hostDataType =
          &Runtime::memoryRef<faabric_datatype_t>(memory, wasmPtr);
        return hostDataType;
    }

    /**
     * We use a trick here to avoid allocating extra memory. Rather than create
     * an actual struct for the MPI_Request, we just use the pointer to hold the
     * value of its ID
     */
    void writeFaasmRequestId(I32 requestPtrPtr, I32 requestId)
    {
        writeMpiResult<int>(requestPtrPtr, requestId);
    }

    /**
     * This uses the same trick, where we read the value of the pointer as the
     * request ID.
     */
    I32 getFaasmRequestId(I32 requestPtrPtr)
    {
        I32 requestId = Runtime::memoryRef<I32>(
          getExecutingWAVMModule()->defaultMemory, requestPtrPtr);
        return requestId;
    }

    faabric_info_t* getFaasmInfoType(I32 wasmPtr)
    {
        faabric_info_t* hostInfoType =
          &Runtime::memoryRef<faabric_info_t>(memory, wasmPtr);
        return hostInfoType;
    }

    faabric_op_t* getFaasmOp(I32 wasmOp)
    {
        faabric_op_t* hostOpType =
          &Runtime::memoryRef<faabric_op_t>(memory, wasmOp);
        return hostOpType;
    }

    template<typename T>
    void writeMpiResult(I32 resPtr, T result)
    {
        T* hostResPtr = &Runtime::memoryRef<T>(memory, resPtr);
        *hostResPtr = result;
    }

    WAVMWasmModule* module;
    Runtime::Memory* memory;
    MpiWorld& world;
    int rank;
};

static thread_local std::unique_ptr<ContextWrapper> ctx = nullptr;

/**
 * Sets up the MPI world. Arguments are argc/argv which are NULL, NULL in our
 * case
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env, "MPI_Init", I32, MPI_Init, I32 a, I32 b)
{
    faabric::Message* call = &ExecutorContext::get()->getMsg();

    // Note - only want to initialise the world on rank zero (or when rank isn't
    // set yet)
    if (call->mpirank() <= 0) {
        SPDLOG_DEBUG("S - MPI_Init (create) {} {}", a, b);

        // Initialise the world
        int worldId = executingContext.createWorld(*call);
        call->set_mpiworldid(worldId);
    } else {
        SPDLOG_DEBUG("S - MPI_Init (join) {} {}", a, b);

        // Join the world
        executingContext.joinWorld(*call);
    }

    ctx = std::make_unique<ContextWrapper>();

    return 0;
}

/**
 * Returns the version of the standard corresponding to the current
 * implementation.
 *
 * TODO not implemented.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Get_version",
                               I32,
                               MPI_Get_version,
                               I32 version,
                               I32 subversion)
{
    MPI_FUNC_ARGS("MPI_Get_version {} {}", version, subversion);
    throw std::runtime_error("MPI_Get_version not implemented!");

    return MPI_SUCCESS;
}

/**
 * Returns the number of ranks in the given communicator
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Comm_size",
                               I32,
                               MPI_Comm_size,
                               I32 comm,
                               I32 resPtr)
{
    MPI_FUNC_ARGS("S - MPI_Comm_size {} {}", comm, resPtr);

    ctx->checkMpiComm(comm);
    ctx->writeMpiResult<int>(resPtr, ctx->world.getSize());

    return MPI_SUCCESS;
}

/**
 * Returns the rank of the caller
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Comm_rank",
                               I32,
                               MPI_Comm_rank,
                               I32 comm,
                               I32 resPtr)
{
    MPI_FUNC_ARGS("S - MPI_Comm_rank {} {}", comm, resPtr);

    ctx->checkMpiComm(comm);
    ctx->writeMpiResult<int>(resPtr, ctx->rank);

    return MPI_SUCCESS;
}

/**
 * Duplicates an existing communicator with all its cached information.
 *
 * TODO not implemented.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Comm_dup",
                               I32,
                               MPI_Comm_dup,
                               I32 comm,
                               I32 newComm)
{
    MPI_FUNC_ARGS("S - MPI_Comm_dup {} {}", comm, newComm);

    throw std::runtime_error("MPI_Comm_dup not implemented!");

    return MPI_SUCCESS;
}

/**
 * Mark a communicator object for deallocation
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Comm_free",
                               I32,
                               MPI_Comm_free,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Comm_free {}", comm);

    // Dealoccation is handled outside of MPI.

    return MPI_SUCCESS;
}

/**
 * Creates new communicators based on colors and keys.
 *
 * TODO not implemented.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Comm_split",
                               I32,
                               MPI_Comm_split,
                               I32 comm,
                               I32 color,
                               I32 key,
                               I32 newComm)
{
    MPI_FUNC_ARGS("S - MPI_Comm_split {} {} {} {}", comm, color, key, newComm);

    throw std::runtime_error("MPI_Comm_split not implemented!");

    return MPI_SUCCESS;
}

/**
 * Returns a valid Fortran communicator handler
 *
 * https://www.open-mpi.org/doc/v4.0/man3/MPI_Comm_c2f.3.php
 * TODO not implemented
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env, "MPI_Comm_c2f", I32, MPI_Comm_c2f, I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Comm_c2f {}", comm);

    throw std::runtime_error("S - MPI_Comm_c2f not implemented!");

    // Implementation note: this function does not return an error value
    // it instead returns a Fortran comm handler (of type MPI_Fint).
    return MPI_SUCCESS;
}

/**
 * Returns a valid C communicator handler
 *
 * https://www.open-mpi.org/doc/v4.0/man3/MPI_Comm_c2f.3.php
 * TODO not implemented
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Comm_f2c",
                               I32,
                               MPI_Comm_f2c,
                               I32 fComm)
{
    MPI_FUNC_ARGS("S - MPI_Comm_f2c {}", fComm);

    throw std::runtime_error("S - MPI_Comm_f2c not implemented!");

    // Implementation note: this function does not return an error value
    // it instead returns an communicator handler (of type MPI_Comm).
    return MPI_SUCCESS;
}

/**
 * Sends a single point-to-point message
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Send",
                               I32,
                               MPI_Send,
                               I32 buffer,
                               I32 count,
                               I32 datatype,
                               I32 destRank,
                               I32 tag,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Send {} {} {} {} {} {}",
                  buffer,
                  count,
                  datatype,
                  destRank,
                  tag,
                  comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);
    auto inputs = Runtime::memoryArrayPtr<uint8_t>(ctx->memory, buffer, count);
    ctx->world.send(ctx->rank, destRank, inputs, hostDtype, count);

    return 0;
}

/**
 * Ready send: the user guarantees that a receive is already posted.
 * TODO not implemented
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Rsend",
                               I32,
                               MPI_Rsend,
                               I32 buffer,
                               I32 count,
                               I32 datatype,
                               I32 destRank,
                               I32 tag,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Rsend {} {} {} {} {} {}",
                  buffer,
                  count,
                  datatype,
                  destRank,
                  tag,
                  comm);

    throw std::runtime_error("MPI_Rsend is not implemented");

    return MPI_SUCCESS;
}

int terminateMpi()
{
    // Destroy the MPI world
    ctx->world.destroy();

    // Null-out the context
    ctx = nullptr;

    return MPI_SUCCESS;
}

/**
 * Sends a single async point-to-point message
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Isend",
                               I32,
                               MPI_Isend,
                               I32 buffer,
                               I32 count,
                               I32 datatype,
                               I32 destRank,
                               I32 tag,
                               I32 comm,
                               I32 requestPtrPtr)
{
    MPI_FUNC_ARGS("S - MPI_Isend {} {} {} {} {} {} {}",
                  buffer,
                  count,
                  datatype,
                  destRank,
                  tag,
                  comm,
                  requestPtrPtr);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);

    auto inputs = Runtime::memoryArrayPtr<uint8_t>(ctx->memory, buffer, count);
    int requestId =
      ctx->world.isend(ctx->rank, destRank, inputs, hostDtype, count);

    ctx->writeFaasmRequestId(requestPtrPtr, requestId);

    return MPI_SUCCESS;
}

/**
 * Returns the number of elements the given MPI_Status corresponds to.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Get_count",
                               I32,
                               MPI_Get_count,
                               I32 statusPtr,
                               I32 datatype,
                               I32 countPtr)
{
    SPDLOG_TRACE("S - MPI_Get_count {} {} {}", statusPtr, datatype, countPtr);

    MPI_Status* status =
      &Runtime::memoryRef<MPI_Status>(ctx->memory, statusPtr);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);
    if (status->bytesSize % hostDtype->size != 0) {
        SPDLOG_ERROR("Incomplete message (bytes {}, datatype size {})",
                     status->bytesSize,
                     hostDtype->size);
        return 1;
    }

    int nVals = status->bytesSize / hostDtype->size;
    ctx->writeMpiResult<int>(countPtr, nVals);

    return MPI_SUCCESS;
}

/**
 * Receives a single point-to-point message.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Recv",
                               I32,
                               MPI_Recv,
                               I32 buffer,
                               I32 count,
                               I32 datatype,
                               I32 sourceRank,
                               I32 tag,
                               I32 comm,
                               I32 statusPtr)
{
    MPI_FUNC_ARGS("S - MPI_Recv {} {} {} {} {} {} {}",
                  buffer,
                  count,
                  datatype,
                  sourceRank,
                  tag,
                  comm,
                  statusPtr);

    ctx->checkMpiComm(comm);
    MPI_Status* status =
      &Runtime::memoryRef<MPI_Status>(ctx->memory, statusPtr);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);
    auto outputs = Runtime::memoryArrayPtr<uint8_t>(ctx->memory, buffer, count);
    ctx->world.recv(sourceRank, ctx->rank, outputs, hostDtype, count, status);

    return 0;
}

/**
 * Sends and receives a message.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Sendrecv",
                               I32,
                               MPI_Sendrecv,
                               I32 sendBuf,
                               I32 sendCount,
                               I32 sendType,
                               I32 destination,
                               I32 sendTag,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 recvType,
                               I32 source,
                               I32 recvTag,
                               I32 comm,
                               I32 statusPtr)
{
    MPI_FUNC_ARGS("S - MPI_Sendrecv {} {} {} {} {} {} {} {} {} {} {} {}",
                  sendBuf,
                  sendCount,
                  sendType,
                  destination,
                  sendTag,
                  recvBuf,
                  recvCount,
                  recvType,
                  source,
                  recvTag,
                  comm,
                  statusPtr);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostSendDtype = ctx->getFaasmDataType(sendType);
    faabric_datatype_t* hostRecvDtype = ctx->getFaasmDataType(recvType);
    MPI_Status* status =
      &Runtime::memoryRef<MPI_Status>(ctx->memory, statusPtr);
    auto hostSendBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, sendBuf, sendCount * hostSendDtype->size);
    auto hostRecvBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, recvBuf, recvCount * hostRecvDtype->size);

    ctx->world.sendRecv(hostSendBuffer,
                        sendCount,
                        hostSendDtype,
                        destination,
                        hostRecvBuffer,
                        recvCount,
                        hostRecvDtype,
                        source,
                        ctx->rank,
                        status);

    return MPI_SUCCESS;
}

/**
 * Receives a single asynchronous point-to-point message.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Irecv",
                               I32,
                               MPI_Irecv,
                               I32 buffer,
                               I32 count,
                               I32 datatype,
                               I32 sourceRank,
                               I32 tag,
                               I32 comm,
                               I32 requestPtrPtr)
{
    MPI_FUNC_ARGS("S - MPI_Irecv {} {} {} {} {} {} {}",
                  buffer,
                  count,
                  datatype,
                  sourceRank,
                  tag,
                  comm,
                  requestPtrPtr);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);
    auto outputs = Runtime::memoryArrayPtr<uint8_t>(ctx->memory, buffer, count);
    int requestId =
      ctx->world.irecv(sourceRank, ctx->rank, outputs, hostDtype, count);

    ctx->writeFaasmRequestId(requestPtrPtr, requestId);

    return MPI_SUCCESS;
}

/**
 * Waits for the asynchronous request to complete
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Wait",
                               I32,
                               MPI_Wait,
                               I32 requestPtrPtr,
                               I32 status)
{
    int requestId = ctx->getFaasmRequestId(requestPtrPtr);

    MPI_FUNC_ARGS("S - MPI_Wait {} {}", requestPtrPtr, requestId);
    ctx->world.awaitAsyncRequest(requestId);

    return MPI_SUCCESS;
}

/**
 * Waits for all given communications to complete
 * TODO not implemented
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Waitall",
                               I32,
                               MPI_Waitall,
                               I32 count,
                               I32 requestArray,
                               I32 statusArray)
{
    MPI_FUNC_ARGS("S - MPI_Waitall {} {} {}", count, requestArray, statusArray);

    throw std::runtime_error("MPI_Waitall is not implemented!");

    return MPI_SUCCESS;
}

/**
 * Waits for any specified send or receive to complete
 * TODO not implemented
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Waitany",
                               I32,
                               MPI_Waitany,
                               I32 count,
                               I32 requestArray,
                               I32 idx,
                               I32 status)
{
    MPI_FUNC_ARGS(
      "S - MPI_Waitany {} {} {} {}", count, requestArray, idx, status);

    throw std::runtime_error("MPI_Waitany is not implemented!");

    return MPI_SUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "MPI_Abort", I32, MPI_Abort, I32 a, I32 b)
{
    MPI_FUNC_ARGS("S - MPI_Abort {} {}", a, b);
    return terminateMpi();
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "MPI_Finalize", I32, MPI_Finalize)
{
    MPI_FUNC("S - MPI_Finalize");
    return terminateMpi();
}

/**
 * Populates the given status with info about an incoming message.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Probe",
                               I32,
                               MPI_Probe,
                               I32 source,
                               I32 tag,
                               I32 comm,
                               I32 statusPtr)
{
    MPI_FUNC_ARGS("S - MPI_Probe {} {} {} {}", source, tag, comm, statusPtr);

    ctx->checkMpiComm(comm);
    MPI_Status* status =
      &Runtime::memoryRef<MPI_Status>(ctx->memory, statusPtr);
    ctx->world.probe(source, ctx->rank, status);

    return MPI_SUCCESS;
}

/**
 * Broadcasts a message. This is called by _both_ senders and receivers of
 * broadcasts.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Bcast",
                               I32,
                               MPI_Bcast,
                               I32 buffer,
                               I32 count,
                               I32 datatype,
                               I32 root,
                               I32 comm)
{
    MPI_FUNC_ARGS(
      "S - MPI_Bcast {} {} {} {} {}", buffer, count, datatype, root, comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);
    auto inputs = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, buffer, count * hostDtype->size);

    ctx->world.broadcast(root,
                         ctx->rank,
                         inputs,
                         hostDtype,
                         count,
                         faabric::mpi::MPIMessage::BROADCAST);

    return MPI_SUCCESS;
}

/**
 * Barrier between all ranks in the given communicator. Called by every rank in
 * the communicator.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env, "MPI_Barrier", I32, MPI_Barrier, I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Barrier {}", comm);

    ctx->checkMpiComm(comm);
    ctx->world.barrier(ctx->rank);

    return MPI_SUCCESS;
}

/**
 * Distributes an array of data between all ranks in the communicator
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Scatter",
                               I32,
                               MPI_Scatter,
                               I32 sendBuf,
                               I32 sendCount,
                               I32 sendType,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 recvType,
                               I32 root,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Scatter {} {} {} {} {} {} {} {}",
                  sendBuf,
                  sendCount,
                  sendType,
                  recvBuf,
                  recvCount,
                  recvType,
                  root,
                  comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostSendDtype = ctx->getFaasmDataType(sendType);
    faabric_datatype_t* hostRecvDtype = ctx->getFaasmDataType(recvType);

    auto hostSendBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, sendBuf, sendCount * hostSendDtype->size);
    auto hostRecvBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, recvBuf, recvCount * hostRecvDtype->size);

    ctx->world.scatter(root,
                       ctx->rank,
                       hostSendBuffer,
                       hostSendDtype,
                       sendCount,
                       hostRecvBuffer,
                       hostRecvDtype,
                       recvCount);

    return MPI_SUCCESS;
}

/**
 * Pulls data from all ranks in a communicator into a single buffer.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Gather",
                               I32,
                               MPI_Gather,
                               I32 sendBuf,
                               I32 sendCount,
                               I32 sendType,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 recvType,
                               I32 root,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Gather {} {} {} {} {} {} {} {}",
                  sendBuf,
                  sendCount,
                  sendType,
                  recvBuf,
                  recvCount,
                  recvType,
                  root,
                  comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostSendDtype = ctx->getFaasmDataType(sendType);
    faabric_datatype_t* hostRecvDtype = ctx->getFaasmDataType(recvType);

    auto hostRecvBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, recvBuf, recvCount * hostRecvDtype->size);
    uint8_t* hostSendBuffer;
    if (isInPlace(sendBuf)) {
        hostSendBuffer = hostRecvBuffer;
    } else {
        hostSendBuffer = Runtime::memoryArrayPtr<uint8_t>(
          ctx->memory, sendBuf, sendCount * hostSendDtype->size);
    }

    ctx->world.gather(ctx->rank,
                      root,
                      hostSendBuffer,
                      hostSendDtype,
                      sendCount,
                      hostRecvBuffer,
                      hostRecvDtype,
                      recvCount);

    return MPI_SUCCESS;
}

/**
 * Each rank gathers data from all other ranks. Results in all seeing the same
 * buffer.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Allgather",
                               I32,
                               MPI_Allgather,
                               I32 sendBuf,
                               I32 sendCount,
                               I32 sendType,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 recvType,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Allgather {} {} {} {} {} {} {}",
                  sendBuf,
                  sendCount,
                  sendType,
                  recvBuf,
                  recvCount,
                  recvType,
                  comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostSendDtype = ctx->getFaasmDataType(sendType);
    faabric_datatype_t* hostRecvDtype = ctx->getFaasmDataType(recvType);

    auto hostRecvBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, recvBuf, recvCount * hostRecvDtype->size);

    // Check if we're in-place
    uint8_t* hostSendBuffer;
    if (isInPlace(sendBuf)) {
        hostSendBuffer = hostRecvBuffer;
    } else {
        hostSendBuffer = Runtime::memoryArrayPtr<uint8_t>(
          ctx->memory, sendBuf, sendCount * hostSendDtype->size);
    }

    ctx->world.allGather(ctx->rank,
                         hostSendBuffer,
                         hostSendDtype,
                         sendCount,
                         hostRecvBuffer,
                         hostRecvDtype,
                         recvCount);

    return MPI_SUCCESS;
}

/**
 * Gathers data from all processes and delivers it to all. Each process may
 * contribute a different amount of data.
 *
 * TODO not implemented.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Allgatherv",
                               I32,
                               MPI_Allgatherv,
                               I32 sendBuf,
                               I32 sendCount,
                               I32 sendType,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 dspls,
                               I32 recvType,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Allgatherv {} {} {} {} {} {} {} {}",
                  sendBuf,
                  sendCount,
                  sendType,
                  recvBuf,
                  recvCount,
                  dspls,
                  recvType,
                  comm);

    throw std::runtime_error("MPI_Allgatherv not implemented!");

    return MPI_SUCCESS;
}

/**
 * Reduces data sent by all ranks in the communicator using the given operator.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Reduce",
                               I32,
                               MPI_Reduce,
                               I32 sendBuf,
                               I32 recvBuf,
                               I32 count,
                               I32 datatype,
                               I32 op,
                               I32 root,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Reduce {} {} {} {} {} {} {}",
                  sendBuf,
                  recvBuf,
                  count,
                  datatype,
                  op,
                  root,
                  comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);

    auto hostRecvBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, recvBuf, count * hostDtype->size);

    // Check if we're working in-place
    uint8_t* hostSendBuffer;
    if (isInPlace(sendBuf)) {
        hostSendBuffer = hostRecvBuffer;
    } else {
        hostSendBuffer = Runtime::memoryArrayPtr<uint8_t>(
          ctx->memory, sendBuf, count * hostDtype->size);
    }

    faabric_op_t* hostOp = ctx->getFaasmOp(op);

    ctx->world.reduce(ctx->rank,
                      root,
                      hostSendBuffer,
                      hostRecvBuffer,
                      hostDtype,
                      count,
                      hostOp);

    return MPI_SUCCESS;
}

/**
 * Combines values and scatters the results.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Reduce_scatter",
                               I32,
                               MPI_Reduce_scatter,
                               I32 sendBuf,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 datatype,
                               I32 op,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Reduce_scatter {} {} {} {} {} {}",
                  sendBuf,
                  recvBuf,
                  recvCount,
                  datatype,
                  op,
                  comm);

    throw std::runtime_error("MPI_Reduce_scatter is not implemented!");

    return MPI_SUCCESS;
}

/**
 * Reduces data from all ranks in the communicator into all ranks, i.e.
 * an all-to-all reduce where each ends up with the same data.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Allreduce",
                               I32,
                               MPI_Allreduce,
                               I32 sendBuf,
                               I32 recvBuf,
                               I32 count,
                               I32 datatype,
                               I32 op,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Allreduce {} {} {} {} {} {}",
                  sendBuf,
                  recvBuf,
                  count,
                  datatype,
                  op,
                  comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);
    faabric_op_t* hostOp = ctx->getFaasmOp(op);

    auto* hostRecvBuffer =
      Runtime::memoryArrayPtr<uint8_t>(ctx->memory, recvBuf, count);

    // Check if we're operating in-place
    uint8_t* hostSendBuffer;
    if (isInPlace(sendBuf)) {
        hostSendBuffer = hostRecvBuffer;
    } else {
        hostSendBuffer =
          Runtime::memoryArrayPtr<uint8_t>(ctx->memory, sendBuf, count);
    }

    ctx->world.allReduce(
      ctx->rank, hostSendBuffer, hostRecvBuffer, hostDtype, count, hostOp);

    return MPI_SUCCESS;
}

/**
 * Computes an inclusive scan (partial reduction). The operation returns,
 * when run on process with rank `i`, the reduction of the values of
 * processes 0, ..., i (inclusive).
 *
 * Reference implementation:
 * https://github.com/open-mpi/ompi/blob/master/ompi/mpi/c/scan.c
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Scan",
                               I32,
                               MPI_Scan,
                               I32 sendBuf,
                               I32 recvBuf,
                               I32 count,
                               I32 datatype,
                               I32 op,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Scan {} {} {} {} {} {}",
                  sendBuf,
                  recvBuf,
                  count,
                  datatype,
                  op,
                  comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostDtype = ctx->getFaasmDataType(datatype);

    auto hostRecvBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, recvBuf, count * hostDtype->size);

    // Check if we're working in-place
    uint8_t* hostSendBuffer;
    if (isInPlace(sendBuf)) {
        hostSendBuffer = hostRecvBuffer;
    } else {
        hostSendBuffer = Runtime::memoryArrayPtr<uint8_t>(
          ctx->memory, sendBuf, count * hostDtype->size);
    }

    faabric_op_t* hostOp = ctx->getFaasmOp(op);

    ctx->world.scan(
      ctx->rank, hostSendBuffer, hostRecvBuffer, hostDtype, count, hostOp);

    return MPI_SUCCESS;
}

/**
 * Sends an all-to-all message.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Alltoall",
                               I32,
                               MPI_Alltoall,
                               I32 sendBuf,
                               I32 sendCount,
                               I32 sendType,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 recvType,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Alltoall {} {} {} {} {} {} {}",
                  sendBuf,
                  sendCount,
                  sendType,
                  recvBuf,
                  recvCount,
                  recvType,
                  comm);

    ctx->checkMpiComm(comm);
    faabric_datatype_t* hostSendDtype = ctx->getFaasmDataType(sendType);
    faabric_datatype_t* hostRecvDtype = ctx->getFaasmDataType(recvType);
    auto hostSendBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, sendBuf, sendCount * hostSendDtype->size);
    auto hostRecvBuffer = Runtime::memoryArrayPtr<uint8_t>(
      ctx->memory, recvBuf, recvCount * hostRecvDtype->size);

    ctx->world.allToAll(ctx->rank,
                        hostSendBuffer,
                        hostSendDtype,
                        sendCount,
                        hostRecvBuffer,
                        hostRecvDtype,
                        recvCount);

    return MPI_SUCCESS;
}

/**
 * All processes send different amount of data to, and receive different
 * amount of data from, all processes.
 *
 * TODO not implemented
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Alltoallv",
                               I32,
                               MPI_Alltoallv,
                               I32 sendBuf,
                               I32 sendCount,
                               I32 sdispls,
                               I32 sendType,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 rdispls,
                               I32 recvType,
                               I32 comm)
{
    MPI_FUNC_ARGS("S - MPI_Alltoallv {} {} {} {} {} {} {} {} {}",
                  sendBuf,
                  sendCount,
                  sdispls,
                  sendType,
                  recvBuf,
                  recvCount,
                  rdispls,
                  recvType,
                  comm);

    throw std::runtime_error("MPI_Alltoallv not implemented!");

    return MPI_SUCCESS;
}

/**
 * Returns the name of this host
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Get_processor_name",
                               I32,
                               MPI_Get_processor_name,
                               I32 buf,
                               I32 bufLen)
{
    MPI_FUNC_ARGS("S - MPI_Get_processor_name {} {}", buf, bufLen);

    const std::string host = faabric::util::getSystemConfig().endpointHost;
    char* key = &Runtime::memoryRef<char>(
      getExecutingWAVMModule()->defaultMemory, (Uptr)buf);
    strcpy(key, host.c_str());

    return MPI_SUCCESS;
}

/**
 * Returns the size of the type.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Type_size",
                               I32,
                               MPI_Type_size,
                               I32 typePtr,
                               I32 res)
{
    MPI_FUNC_ARGS("S - MPI_Type_size {} {}", typePtr, res);

    faabric_datatype_t* hostType = ctx->getFaasmDataType(typePtr);
    ctx->writeMpiResult<int>(res, hostType->size);

    return MPI_SUCCESS;
}

/**
 * Allocates memory on this host (equivalent to a malloc)
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Alloc_mem",
                               I32,
                               MPI_Alloc_mem,
                               I32 memSize,
                               I32 info,
                               I32 resPtrPtr)
{
    MPI_FUNC_ARGS("S - MPI_Alloc_mem {} {} {}", memSize, info, resPtrPtr);

    faabric_info_t* hostInfo = ctx->getFaasmInfoType(info);
    if (hostInfo->id != FAABRIC_INFO_NULL) {
        throw std::runtime_error("Non-null info not supported");
    }

    // Create the new memory region
    WAVMWasmModule* module = getExecutingWAVMModule();
    U32 pageAlignedSize = roundUpToWasmPageAligned(memSize);
    U32 mappedWasmPtr = module->growMemory(pageAlignedSize);

    // Write the result to the wasm memory (note that the argument passed to the
    // function is a pointer to a pointer)
    I32* hostResPtr =
      &Runtime::memoryRef<I32>(module->defaultMemory, resPtrPtr);
    *hostResPtr = mappedWasmPtr;

    return MPI_SUCCESS;
}

/**
 * Makes a new communicator to which Cartesian topology information has been
 * attached.
 * Note: In MPI, memory is allocated from within the function call, that's
 * why we allocate it here.
 *
 * Reference implementation:
 * https://github.com/open-mpi/ompi/blob/master/ompi/mca/topo/base/topo_base_cart_create.c
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Cart_create",
                               I32,
                               MPI_Cart_create,
                               I32 commOld,
                               I32 ndims,
                               I32 dims,
                               I32 periods,
                               I32 reorder,
                               I32 newCommPtrPtr)
{
    MPI_FUNC_ARGS("S - MPI_Cart_create {} {} {} {} {} {}",
                  commOld,
                  ndims,
                  dims,
                  periods,
                  reorder,
                  newCommPtrPtr);

    // Allocate new faabric_communicator_t object
    I32 memSize = sizeof(faabric_communicator_t);
    U32 pageAlignedSize = roundUpToWasmPageAligned(memSize);

    U32 mappedWasmPtr = ctx->module->growMemory(pageAlignedSize);

    // Write the value to wasm memory
    I32* hostCommPtr = &Runtime::memoryRef<I32>(ctx->memory, newCommPtrPtr);
    *hostCommPtr = mappedWasmPtr;

    // Cast the original communicator
    faabric_communicator_t* origComm =
      &Runtime::memoryRef<faabric_communicator_t>(ctx->memory, commOld);

    // Populate the new object
    faabric_communicator_t* newComm =
      &Runtime::memoryRef<faabric_communicator_t>(ctx->memory, *hostCommPtr);
    *newComm = *origComm;

    return MPI_SUCCESS;
}

/**
 * Determines process rank in communicator given Cartesian location.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Cart_rank",
                               I32,
                               MPI_Cart_rank,
                               I32 comm,
                               I32 coords,
                               I32 rankPtr)
{
    MPI_FUNC_ARGS("S - MPI_Cart_rank {} {} {}", comm, coords, rankPtr);

    int* coordsArray = Runtime::memoryArrayPtr<int>(
      ctx->memory, (Uptr)coords, MPI_CART_MAX_DIMENSIONS);

    int rank;
    ctx->world.getRankFromCoords(&rank, coordsArray);
    ctx->writeMpiResult<int>(rankPtr, rank);

    return MPI_SUCCESS;
}

/**
 * Retrieves the Cartesian topology information associated with a
 * communicator.
 *
 * MPI Topologies are pointless in a serverless environment. Therefore
 * we return default values (2dim grid) basing on the current world size.
 *
 * In particular we define a 2-dim grid with as many processors, leaving
 * the rest as MPI_UNDEFINED.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Cart_get",
                               I32,
                               MPI_Cart_get,
                               I32 comm,
                               I32 maxdims,
                               I32 dims,
                               I32 periods,
                               I32 coords)
{
    MPI_FUNC_ARGS(
      "S - MPI_Cart_get {} {} {} {} {}", comm, maxdims, dims, periods, coords);

    // If the provided value is lower we error out. Otherwise we will just
    // use the first <MPI_CART_MAX_DIMENSIONS> array positions.
    if (maxdims < MPI_CART_MAX_DIMENSIONS) {
        SPDLOG_ERROR("Unexpected number of max. dimensions: {}", maxdims);
        throw std::runtime_error("Bad dimensions in MPI_Cart_get");
    }

    // Allocate the vectors in wasm memory
    int* dimsArray =
      Runtime::memoryArrayPtr<int>(ctx->memory, (Uptr)dims, (Uptr)maxdims);
    int* periodsArray =
      Runtime::memoryArrayPtr<int>(ctx->memory, (Uptr)periods, (Uptr)maxdims);
    int* coordsArray =
      Runtime::memoryArrayPtr<int>(ctx->memory, (Uptr)coords, (Uptr)maxdims);

    ctx->world.getCartesianRank(
      ctx->rank, maxdims, dimsArray, periodsArray, coordsArray);

    return MPI_SUCCESS;
}

/**
 * Returns the shifted source and destination ranks, given a shift direction
 * and amount.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Cart_shift",
                               I32,
                               MPI_Cart_shift,
                               I32 comm,
                               I32 direction,
                               I32 disp,
                               I32 sourceRank,
                               I32 destRank)
{
    MPI_FUNC_ARGS("S - MPI_Cart_shift {} {} {} {} {}",
                  comm,
                  direction,
                  disp,
                  sourceRank,
                  destRank);

    int hostSourceRank, hostDestRank;

    ctx->world.shiftCartesianCoords(
      ctx->rank, direction, disp, &hostSourceRank, &hostDestRank);

    ctx->writeMpiResult<int>(sourceRank, hostSourceRank);
    ctx->writeMpiResult<int>(destRank, hostDestRank);

    return MPI_SUCCESS;
}

/**
 * Creates a user-defined combination function handle.
 *
 * TODO not implemented.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Op_create",
                               I32,
                               MPI_Op_create,
                               I32 userFn,
                               I32 commute,
                               I32 op)
{
    MPI_FUNC_ARGS("S - MPI_Op_create {} {} {}", userFn, commute, op);

    throw std::runtime_error("MPI_Op_create not implemented!");

    return MPI_SUCCESS;
}

/**
 * Frees a user-defined combination function handle.
 *
 * TODO not implemented.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env, "MPI_Op_free", I32, MPI_Op_free, I32 op)
{
    MPI_FUNC_ARGS("S - MPI_Op_free {}", op);

    throw std::runtime_error("MPI_Op_free not implemented!");

    return MPI_SUCCESS;
}

/**
 * Creates a shared memory region (i.e. a chunk of Faasm state)
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Win_create",
                               I32,
                               MPI_Win_create,
                               I32 basePtr,
                               I32 size,
                               I32 dispUnit,
                               I32 info,
                               I32 comm,
                               I32 winPtrPtr)
{
    MPI_FUNC_ARGS("S - MPI_Win_create {} {} {} {} {} {}",
                  basePtr,
                  size,
                  dispUnit,
                  info,
                  comm,
                  winPtrPtr);

    throw std::runtime_error("MPI_Win_create is not implemented!");

    return MPI_SUCCESS;
}

/**
 * Special type of barrier invoked to ensure all RMA operations have completed.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Win_fence",
                               I32,
                               MPI_Win_fence,
                               I32 assert,
                               I32 winPtr)
{
    MPI_FUNC_ARGS("S - MPI_Win_fence {} {}", assert, winPtr);

    throw std::runtime_error("MPI_Win_fence is not implemented!");

    return MPI_SUCCESS;
}

/**
 * One-sided get RDMA.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Get",
                               I32,
                               MPI_Get,
                               I32 recvBuf,
                               I32 recvCount,
                               I32 recvType,
                               I32 sendRank,
                               I32 sendOffset,
                               I32 sendCount,
                               I32 sendType,
                               I32 winPtr)
{
    MPI_FUNC_ARGS("S - MPI_Get {} {} {} {} {} {} {} {}",
                  recvBuf,
                  recvCount,
                  recvType,
                  sendRank,
                  sendOffset,
                  sendCount,
                  sendType,
                  winPtr);

    throw std::runtime_error("MPI_Get is not implemented!");

    return MPI_SUCCESS;
}

/**
 * One-sided write to shared memory.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Put",
                               I32,
                               MPI_Put,
                               I32 sendBuf,
                               I32 sendCount,
                               I32 sendType,
                               I32 recvRank,
                               I32 recvOffset,
                               I32 recvCount,
                               I32 recvType,
                               I32 winPtr)
{
    MPI_FUNC_ARGS("S - MPI_Put {} {} {} {} {} {} {} {}",
                  sendBuf,
                  sendCount,
                  sendType,
                  recvRank,
                  recvOffset,
                  recvCount,
                  recvType,
                  winPtr);

    throw std::runtime_error("MPI_Put is not implemented!");

    return MPI_SUCCESS;
}

/**
 * Cleans up the given window
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Win_free",
                               I32,
                               MPI_Win_free,
                               I32 winPtr)
{
    MPI_FUNC_ARGS("S - MPI_Win_free {}", winPtr);

    throw std::runtime_error("MPI_Win_free is not implemented!");

    return MPI_SUCCESS;
}

/**
 * Returns the value for a given attribute of a window.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Win_get_attr",
                               I32,
                               MPI_Win_get_attr,
                               I32 winPtr,
                               I32 attrKey,
                               I32 attrResPtrPtr,
                               I32 flagResPtr)
{
    MPI_FUNC_ARGS("S - MPI_Win_get_attr {} {} {} {}",
                  winPtr,
                  attrKey,
                  attrResPtrPtr,
                  flagResPtr);

    throw std::runtime_error("MPI_Win_get_attr is not implemented!");

    return MPI_SUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Free_mem",
                               I32,
                               MPI_Free_mem,
                               I32 basePtr)
{
    MPI_FUNC_ARGS("S - MPI_Free_mem {}", basePtr);

    // Can ignore freeing memory (as we do with munmap etc.)

    return MPI_SUCCESS;
}

/**
 * Frees a communication request object.
 * TODO not implemented
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Request_free",
                               I32,
                               MPI_Request_free,
                               I32 requestPtr)
{
    MPI_FUNC_ARGS("S - MPI_Request_free {}", requestPtr);

    throw std::runtime_error("MPI_Request_free is not implemented!");

    return MPI_SUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Type_contiguous",
                               I32,
                               MPI_Type_contiguous,
                               I32 count,
                               I32 oldDatatypePtr,
                               I32 newDatatypePtrPtr)
{
    MPI_FUNC_ARGS("S - MPI_Type_contiguous {} {} {}",
                  count,
                  oldDatatypePtr,
                  newDatatypePtrPtr);

    return MPI_SUCCESS;
}

/**
 * Frees a data type
 *
 * TODO not implemented.
 */
WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Type_free",
                               I32,
                               MPI_Type_free,
                               I32 datatype)
{
    MPI_FUNC_ARGS("S - MPI_Type_free {}", datatype);

    throw std::runtime_error("MPI_Type_free is not implemented!");

    return MPI_SUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env,
                               "MPI_Type_commit",
                               I32,
                               MPI_Type_commit,
                               I32 datatypePtrPtr)
{
    MPI_FUNC_ARGS("S - MPI_Type_commit {}", datatypePtrPtr);

    return MPI_SUCCESS;
}

WAVM_DEFINE_INTRINSIC_FUNCTION(env, "MPI_Wtime", F64, MPI_Wtime)
{
    MPI_FUNC("S - MPI_Wtime");

    double t = ctx->world.getWTime();
    return (F64)t;
}

void mpiLink() {}
}
