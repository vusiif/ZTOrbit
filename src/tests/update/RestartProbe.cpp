// Restart probe: the helper starts this executable after a successful
// transaction (unless --no-restart is used). It writes a marker into its working
// directory, which is the fake installation directory of the transaction test.
#include <windows.h>

int main()
{
    HANDLE file = CreateFileW(L"restart-probe-ran.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, "started", 7, &written, nullptr);
        CloseHandle(file);
    }
    return 0;
}
