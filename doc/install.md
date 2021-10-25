# libmultiprocess Installation

Installation currently requires Cap'n Proto:

```sh
apt install libcapnp-dev capnproto
brew install capnp cmake
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
