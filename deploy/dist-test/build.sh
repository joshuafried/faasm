#!/bin/bash

set -e

THIS_DIR=$(dirname $(readlink -f $0))
export PROJ_ROOT=${THIS_DIR}/../..
pushd ${PROJ_ROOT} >> /dev/null

export FAASM_BUILD_MOUNT=/build/faasm
export FAASM_CODE_MOUNT=/usr/local/code/faasm
export FAASM_CONAN_MOUNT=/root/.conan

# Run the build
docker compose \
    run \
    --rm \
    faasm-cli \
    /usr/local/code/faasm/deploy/dist-test/build_internal.sh

popd >> /dev/null
