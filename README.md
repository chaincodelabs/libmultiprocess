# libmultiprocess

## Summary

C++ library and code generator making it easy to call functions and reference objects in different processes.

## Description

Given an interface description of an object with one or more methods, libmultiprocess generates:

* A C++ `ProxyClient` class with an implementation of each interface method that sends a request over a socket, waits for a response, and returns the result.
* A C++ `ProxyServer` class that listens for requests over a socket and calls a wrapped C++ object implementating the same interface to actually execute the requests.

The function call ⇆ request translation supports input and output arguments, standard types like `unique_ptr`, `vector`, `map`, and `optional`, and bidirectional calls between processes through interface pointer and `std::function` arguments.

If the wrapped C++ object inherits from an abstract base class declaring virtual methods, the generated `ProxyClient` objects can inherit from the same class, allowing interprocess calls to replace local calls without changes to existing code.

There is also optional support for thread mapping, so each thread making interprocess calls can have a dedicated thread processing requests from it, and callbacks from processing threads are executed on corresponding request threads (so recursive mutexes and thread names function as expected in callbacks).

## Example

A simple interface description can be found at [test/src/mp/test/foo.capnp](test/src/mp/test/foo.capnp), implementation in [test/src/mp/test/foo.h](test/src/mp/test/foo.h), and usage in [test/src/mp/test/test.cpp](test/src/mp/test/test.cpp).

## Future directions

_libmultiprocess_ uses the [Cap'n Proto](https://capnproto.org) interface description language and protocol, but it could be extended or changed to use a different IDL/protocol like [gRPC](https://grpc.io). The nice thing about _Cap'n Proto_ compared to _gRPC_ and most other lower level protocols is that it allows interface pointers (_Services_ in gRPC parlance) to be passed as method arguments and return values, so object references and bidirectional requests work out of the box. Supporting a lower-level protocol would require writing adding maps and tracking code to proxy objects.

_libmultiprocess_ is currently compatible with sandboxing but could add platform-specific sandboxing support or integration with a sandboxing library like [SAPI](https://github.com/google/sandboxed-api).

## Installation

Installation currently requires Cap'n Proto:

```sh
apt install libcapnp-dev capnproto
brew install capnp
dnf install capnproto
```

Installation steps are:

```sh
mkdir build
cd build
cmake ..
make
make check # Optionally build and run tests
make install
```
