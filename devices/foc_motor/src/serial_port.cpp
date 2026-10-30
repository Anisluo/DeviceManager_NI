#include "transport.hpp"

#include <stdexcept>

#ifdef _WIN32
#  include <windows.h>

SerialPort::SerialPort(const std::string& port, int baud) : name_(port + " @ " + std::to_string(baud))
{
    std::string path = "\\\\.\\" + port;                       // COM10+ needs the device namespace
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("无法打开串口 " + port + "（不存在或被其他程序占用）");
    DCB dcb{};
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = DWORD(baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fOutX = dcb.fInX = FALSE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        throw std::runtime_error("串口 " + port + " 不支持波特率 " + std::to_string(baud));
    }
    SetupComm(h, 1 << 16, 1 << 16);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    h_ = h;
}

SerialPort::~SerialPort()
{
    if (h_) CloseHandle(HANDLE(h_));
}

void SerialPort::write(const uint8_t* data, size_t n)
{
    COMMTIMEOUTS t{};
    t.WriteTotalTimeoutConstant = 500;
    SetCommTimeouts(HANDLE(h_), &t);
    DWORD done = 0;
    if (!WriteFile(HANDLE(h_), data, DWORD(n), &done, nullptr) || done != n)
        throw std::runtime_error("串口写入失败");
}

size_t SerialPort::read(uint8_t* buf, size_t max, int timeout_ms)
{
    // Wait up to timeout_ms for the first byte, then return what has arrived.
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = DWORD(timeout_ms > 0 ? timeout_ms : 1);
    SetCommTimeouts(HANDLE(h_), &t);
    DWORD got = 0;
    if (!ReadFile(HANDLE(h_), buf, DWORD(max), &got, nullptr))
        throw std::runtime_error("串口读取失败（设备已拔出？）");
    return got;
}

void SerialPort::flush_input()
{
    PurgeComm(HANDLE(h_), PURGE_RXCLEAR);
}

std::vector<std::string> SerialPort::list()
{
    std::vector<std::string> out;
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        char name[256], value[256];
        DWORD nlen = sizeof name, vlen = sizeof value, type = 0;
        if (RegEnumValueA(key, i, name, &nlen, nullptr, &type, reinterpret_cast<BYTE*>(value), &vlen) != ERROR_SUCCESS)
            break;
        if (type == REG_SZ) out.emplace_back(value);
    }
    RegCloseKey(key);
    return out;
}

#else  // POSIX
#  include <dirent.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <termios.h>
#  include <unistd.h>

static speed_t baud_const(int b)
{
    switch (b) {
    case 9600: return B9600;     case 19200: return B19200;   case 38400: return B38400;
    case 57600: return B57600;   case 115200: return B115200; case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
    default: throw std::runtime_error("unsupported baud rate " + std::to_string(b));
    }
}

SerialPort::SerialPort(const std::string& port, int baud) : name_(port + " @ " + std::to_string(baud))
{
    fd_ = ::open(port.c_str(), O_RDWR | O_NOCTTY);
    if (fd_ < 0) throw std::runtime_error("cannot open " + port);
    termios tio{};
    tcgetattr(fd_, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_const(baud));
    cfsetospeed(&tio, baud_const(baud));
    tio.c_cflag |= CLOCAL | CREAD;
    tcsetattr(fd_, TCSANOW, &tio);
    tcflush(fd_, TCIOFLUSH);
}

SerialPort::~SerialPort() { if (fd_ >= 0) ::close(fd_); }

void SerialPort::write(const uint8_t* data, size_t n)
{
    while (n) {
        ssize_t w = ::write(fd_, data, n);
        if (w <= 0) throw std::runtime_error("serial write failed");
        data += w; n -= size_t(w);
    }
}

size_t SerialPort::read(uint8_t* buf, size_t max, int timeout_ms)
{
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, timeout_ms) <= 0) return 0;
    ssize_t r = ::read(fd_, buf, max);
    return r > 0 ? size_t(r) : 0;
}

void SerialPort::flush_input() { tcflush(fd_, TCIFLUSH); }

std::vector<std::string> SerialPort::list()
{
    std::vector<std::string> out;
    if (DIR* d = opendir("/dev")) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("ttyUSB", 0) == 0 || n.rfind("ttyACM", 0) == 0) out.push_back("/dev/" + n);
        }
        closedir(d);
    }
    return out;
}
#endif
