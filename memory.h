#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

#include <cstdint>
#include <string_view>
#include <iostream>
#include <thread>
#include <vector>

class Memory
{
private:
    std::uintptr_t processId = 0;
    HANDLE processHandle = nullptr;

    void CloseHandleSafe(HANDLE handle) const noexcept
    {
        if (handle && !::CloseHandle(handle))
        {
            std::cerr << "[-] Failed to close handle, error code: " << GetLastError() << std::endl;
        }
    }

public:
    Memory(const std::string_view processName) noexcept
    {
        std::cout << "[+] Memory constructor called, finding process handle: " << processName << std::endl;

        while (processId == 0)
        {
            PROCESSENTRY32 entry = { sizeof(PROCESSENTRY32) };
            const HANDLE snapShot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapShot == INVALID_HANDLE_VALUE)
            {
                std::cerr << "[-] Failed to create process snapshot, error code: " << GetLastError() << std::endl;
                std::exit(EXIT_FAILURE);
            }

            while (::Process32Next(snapShot, &entry))
            {
                if (processName == entry.szExeFile)
                {
                    processId = entry.th32ProcessID;
                    processHandle = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
                    if (!processHandle)
                    {
                        std::cerr << "[-] Failed to open process handle, error code: " << GetLastError() << std::endl;
                        std::exit(EXIT_FAILURE);
                    }
                    std::cout << "[+] Process handle found" << std::endl;
                    break;
                }
            }

            CloseHandleSafe(snapShot);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    ~Memory()
    {
        std::cout << "[-] Memory destructor called" << std::endl;
        CloseHandleSafe(processHandle);
    }

    std::uintptr_t GetModuleAddress(const std::string_view moduleName) const noexcept
    {
        MODULEENTRY32 entry = { sizeof(MODULEENTRY32) };
        const HANDLE snapShot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, processId);
        if (snapShot == INVALID_HANDLE_VALUE)
        {
            std::cerr << "[-] Failed to create module snapshot, error code: " << GetLastError() << std::endl;
            return 0;
        }

        std::uintptr_t result = 0;
        while (::Module32Next(snapShot, &entry))
        {
            if (moduleName == entry.szModule)
            {
                result = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                break;
            }
        }

        CloseHandleSafe(snapShot);

        if (result == 0)
        {
            std::cerr << "[-] Module not found" << std::endl;
            std::exit(EXIT_FAILURE);
        }

        return result;
    }

    template <typename T>
    void ReadChar(const std::uintptr_t& address, T* value, int size) const noexcept
    {
        if (!::ReadProcessMemory(processHandle, reinterpret_cast<const void*>(address), value, sizeof(T) * size, nullptr))
        {
            std::cerr << "[-] Failed to read process memory, error code: " << GetLastError() << std::endl;
        }
    }

    template <typename T>
    T Read(const std::uintptr_t& address) const noexcept
    {
        T value = {};
        if (!::ReadProcessMemory(processHandle, reinterpret_cast<const void*>(address), &value, sizeof(T), nullptr))
        {
            std::cerr << "[-] Failed to read process memory, error code: " << GetLastError() << std::endl;
        }
        return value;
    }

    template <typename T>
    T Read(const std::uintptr_t& address, const std::vector<uintptr_t>& offsets) const noexcept
    {
        uintptr_t currentAddress = address;

        for (size_t i = 0; i < offsets.size() - 1; ++i)
        {
            if (!::ReadProcessMemory(processHandle, reinterpret_cast<const void*>(currentAddress + offsets[i]), &currentAddress, sizeof(uintptr_t), nullptr))
            {
                std::cerr << "[-] Failed to read process memory, error code: " << GetLastError() << std::endl;
            }
        }

        T value = {};
        if (!::ReadProcessMemory(processHandle, reinterpret_cast<const void*>(currentAddress + offsets.back()), &value, sizeof(T), nullptr))
        {
            std::cerr << "[-] Failed to read process memory, error code: " << GetLastError() << std::endl;
        }

        return value;
    }

    template <typename T>
    void Write(const std::uintptr_t& address, const T& value) const noexcept
    {
        if (!::WriteProcessMemory(processHandle, reinterpret_cast<void*>(address), &value, sizeof(T), nullptr))
        {
            std::cerr << "[-] Failed to write process memory, error code: " << GetLastError() << std::endl;
        }
    }

    template <size_t N>
    void Write(const std::uintptr_t& address, const char(&value)[N]) const noexcept
    {
        if (!::WriteProcessMemory(processHandle, reinterpret_cast<void*>(address), value, N, nullptr))
        {
            std::cerr << "[-] Failed to write process memory, error code: " << GetLastError() << std::endl;
        }
    }

    std::uintptr_t GetProcessId() const noexcept
    {
        return processId;
    }

    std::uintptr_t GetHandleAddress() const noexcept
    {
        return reinterpret_cast<uintptr_t>(processHandle);
    }

    void PatchEx(BYTE* dst, BYTE* src, unsigned int size) const
    {
        DWORD oldProtect;
        ::VirtualProtectEx(processHandle, dst, size, PAGE_EXECUTE_READWRITE, &oldProtect);
        ::WriteProcessMemory(processHandle, dst, src, size, nullptr);
        ::VirtualProtectEx(processHandle, dst, size, oldProtect, &oldProtect);
    }

    void NopEx(BYTE* dst, unsigned int size) const
    {
        std::vector<BYTE> nopArray(size, 0x90);
        PatchEx(dst, nopArray.data(), size);
    }
};