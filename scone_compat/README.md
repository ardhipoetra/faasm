# SCONE-compatible faasm

This document guides you through running a sconified faasm runtime. 
All the necessary files should be located in `scone_compat` directory. 

## Preparation

As a prerequisite, you need access to the following image : 

- redis: registry.scontain.com/sconecuratedimages/apps:redis-6.2.6-bullseye
- minio: registry.scontain.com/ardhipoetra/registry/cloudskin:minio
- faasm: registry.scontain.com/ardhipoetra/registry/cloudskin:faasm

Please get in touch with Scone to get access to the first one and me to the latter two. 

Also, you need to have an SGX-capable environment. Refer to https://sconedocs.github.io/installation/ for details. 

Lastly, you need to have docker-compose with buildkit enabled. 

## Build the image

You could build the latter two images on your own. For minio image (see `Dockerfile.minio`), we use a golang-supported scone image as the base. For faasm image (`Dockerfile.faasm`), we use a scone runtime based on alpine3.16. If you want to build your own image, please gain access to those images. 

A simple build can be done like the following : 
```
docker build -t registry.scontain.com/ardhipoetra/registry/cloudskin:faasm -f Dockerfile.faasm . --no-cache
```

Similar command could be used to build `minio` image too.

## Run the containers

Make sure you adjust your environment by modifying `.env` file. Please make sure the ports are available on the host.

Containers can be quickly built by : 
```exe
docker compose up -d
```

On the other hand, you can take them down by : 
```
docker compose down
```

## Preparing faasm interaction

TLDR:
```
 ./gen_ini.sh 
 docker cp faasm.ini faasm-dev-scone-cpp-1:/
 docker exec -it -e FAASM_INI_FILE=/faasm.ini faasm-dev-scone-cpp-1 /bin/bash
```

In this example, suppose we want to interact with our freshly-deployed faasm cluster. Here, we can use the `faasm-dev-scone-cpp-1` image based on `faasm.azurecr.io/cpp-sysroot:0.3.1`.

First, we need to generate the configuration file. This can be done by executing `gen_ini.sh` as shown below.
```
# pwd
/home/ubuntu/faasm/scone_compat
# ./gen_ini.sh 
Done generating faasm.ini from /home/ubuntu/faasm/scone_compat/.env
```

Note that this script is barely a minimal replacement for the `inv` provided by faasm for generating `.ini` file. Please don't count on it and double-check the resulting file. 

Then, copy the configuration file to cpp image: 
```
 docker cp faasm.ini faasm-dev-scone-cpp-1:/
```

Finally, go into the cpp container and start working from there:
```
# docker exec -it -e FAASM_INI_FILE=/faasm.ini faasm-dev-scone-cpp-1 /bin/bash
----------------------------------
CPP CLI
Version: 0.3.1
Project root: /code/cpp/bin/..
Mode: container
----------------------------------

(cpp) root@02dcf4e5cc1e:/code/cpp# 
```

## Interacting with faasm

This part should be the same as vanilla faasm. Assuming you have correct configurations. Typically, one needs to _compile_, _upload_, and _invoke_, in that specific order. See example below : 

```
(cpp) root@b5f0be9028ab:/code/cpp# inv func.compile demo echo
cmake -GNinja -DCMAKE_TOOLCHAIN_FILE=/usr/local/faasm/toolchain/tools/WasiToolchain.cmake -DCMAKE_BUILD_TYPE=Release /code/cpp/func
-- Faasm building STATIC libraries
-- Faasm building target wasm32-wasi
System is unknown to cmake, create:
Platform/Wasm to use this system, please post your config file on discourse.cmake.org so it can be added to cmake
Your CMakeCache.txt file was copied to CopyOfCMakeCache.txt. Please post that file on discourse.cmake.org.
-- Detected wasm build (sysroot=/usr/local/faasm/llvm-sysroot)
-- Configuring done
-- Generating done
-- Build files have been written to: /code/cpp/build/func
ninja: no work to do.
(cpp) root@b5f0be9028ab:/code/cpp# inv func.upload demo echo
Response (200): Function upload complete

(cpp) root@b5f0be9028ab:/code/cpp# inv func.invoke demo echo
Success:
Nothing to echo
(cpp) root@b5f0be9028ab:/code/cpp#
```

For now, just ignore the warnings. The startup time also increased due to running on top of SGX. 