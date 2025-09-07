#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#include "utils.h"

const char* GetErrorDescription(int errorCode) {
    switch (errorCode) {
        case WSAECONNRESET: return "WSAECONNRESET";
        case WSAECONNABORTED: return "WSAECONNABORTED";
        case WSAENETDOWN: return "WSAENETDOWN";
        case WSAETIMEDOUT: return "WSAETIMEDOUT";
        case WSAENOTCONN: return "WSAENOTCONN";
        case WSAEWOULDBLOCK: return "WSAEWOULDBLOCK";
        case WSAEINVAL: return "WSAEINVAL";
        case WSAENOTSOCK: return "WSAENOTSOCK";
        case WSAEFAULT: return "WSAEFAULT";
        case WSAEINTR: return "WSAEINTR";
        case WSAEMSGSIZE: return "WSAEMSGSIZE";
        default: return "UNKNOWN";
    }
}

