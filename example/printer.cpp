// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fstream>
#include <init.capnp.h>
#include <init.capnp.proxy-types.h>
#include <init.h>
#include <iostream>
#include <memory>
#include <mp/proxy-io.h>
#include <printer.h>
#include <stdexcept>

class PrinterImpl : public Printer
{
public:
    void print(const std::string& message) override { std::cout << "mpprinter: " << message << std::endl; }
};

class InitImpl : public Init
{
public:
    std::unique_ptr<Printer> makePrinter() override { return std::make_unique<PrinterImpl>(); }
};

void LogPrint(bool raise, const std::string& message)
{
    if (raise) throw std::runtime_error(message);
    std::ofstream("debug.log", std::ios_base::app) << message << std::endl;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cout << "Usage: mpprinter <fd>\n";
        return 1;
    }
    mp::EventLoop loop("mpprinter", LogPrint);
    int fd = std::stoi(argv[1]);
    std::unique_ptr<Init> init = std::make_unique<InitImpl>();
    mp::ServeStream<InitInterface>(loop, fd, *init);
    loop.loop();
    return 0;
}
