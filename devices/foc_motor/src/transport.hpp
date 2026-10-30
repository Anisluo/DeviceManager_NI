// transport.hpp — byte stream to the motor controller (serial port or simulator).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Transport {
public:
    virtual ~Transport() = default;
    virtual void write(const uint8_t* data, size_t n) = 0;
    // Reads up to `max` bytes, waiting at most timeout_ms for the first one. Returns 0 on timeout.
    virtual size_t read(uint8_t* buf, size_t max, int timeout_ms) = 0;
    virtual void flush_input() {}
    virtual std::string name() const = 0;
};

class SerialPort : public Transport {
public:
    SerialPort(const std::string& port, int baud);
    ~SerialPort() override;
    void write(const uint8_t* data, size_t n) override;
    size_t read(uint8_t* buf, size_t max, int timeout_ms) override;
    void flush_input() override;
    std::string name() const override { return name_; }

    static std::vector<std::string> list();

private:
    std::string name_;
#ifdef _WIN32
    void* h_ = nullptr;
#else
    int fd_ = -1;
#endif
};
