# libmultiprocess Design

Given an interface description of an object with one or more methods, libmultiprocess generates:

* A C++ `ProxyClient` class with an implementation of each interface method that sends a request over a socket, waits for a response, and returns the result.
* A C++ `ProxyServer` class that listens for requests over a socket and calls a wrapped C++ object implementating the same interface to actually execute the requests.

The function call ⇆ request translation supports input and output arguments, standard types like `unique_ptr`, `vector`, `map`, and `optional`, and bidirectional calls between processes through interface pointer and `std::function` arguments.

If the wrapped C++ object inherits from an abstract base class declaring virtual methods, the generated `ProxyClient` objects can inherit from the same class, allowing interprocess calls to replace local calls without changes to existing code.

There is also optional support for thread mapping, so each thread making interprocess calls can have a dedicated thread processing requests from it, and callbacks from processing threads are executed on corresponding request threads (so recursive mutexes and thread names function as expected in callbacks).

## Interface descriptions

As explained in the [usage](usage.md) document, interface descriptions need to be consumed both by the _libmultiprocess_ code generator, and by C++ code that calls and implements the interfaces. The C++ code only needs to know about C++ arguments and return types, while the code generator only needs to know about capnp arguments and return types, but both need to know class and method names, so the corresponding `.h` and `.capnp` source files contain some of the same information, and have to be kept in sync manually when methods or parameters change. Despite the redundancy, reconciling the interface definitions is designed to be _straightforward_ and _safe_. _Straightforward_ because there is no need to write manual serialization code or use awkward intermediate types like [`UniValue`](https://github.com/bitcoin/bitcoin/blob/master/src/univalue/include/univalue.h) instead of native types. _Safe_ because if there are any inconsistencies between API and data definitions (even minor ones like using a narrow int data type for a wider int API input), there are errors at build time instead of errors or bugs at runtime.

In the future, it would be possible to combine API and data definitions together using [C++ attributes](https://en.cppreference.com/w/cpp/language/attributes). To do this we would add attributes to the API definition files, and then generate the data definitions from the API definitions and attributes. I didn't take this approach mostly because it would be extra work, but also because until c++ standardizes reflection, this would require either hooking into compiler APIs like https://github.com/RosettaCommons/binder, or parsing c++ code manually like http://www.swig.org/.

## What is `kj`?

KJ is a concurrency framework [bundled with
capnproto](https://capnproto.org/cxxrpc.html#kj-concurrency-framework); it is used as a
basis in this library to construct the event-loop necessary to service IPC requests.

## Future directions

_libmultiprocess_ uses the [Cap'n Proto](https://capnproto.org) interface description language and protocol, but it could be extended or changed to use a different IDL/protocol like [gRPC](https://grpc.io). The nice thing about _Cap'n Proto_ compared to _gRPC_ and most other lower level protocols is that it allows interface pointers (_Services_ in gRPC parlance) to be passed as method arguments and return values, so object references and bidirectional requests work out of the box. Supporting a lower-level protocol would require writing adding maps and tracking code to proxy objects.

_libmultiprocess_ is currently compatible with sandboxing but could add platform-specific sandboxing support or integration with a sandboxing library like [SAPI](https://github.com/google/sandboxed-api).
