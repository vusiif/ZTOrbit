// Test stub for the update helper. It records that it was started in its
// working directory and exits successfully; the controller tests use it to prove
// that a fully verified download and staged plan are the only things that can
// trigger the quit request.
#include <windows.h>

int main()
{
    HANDLE file = CreateFileW(L"fake-helper-ran.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, "ran", 3, &written, nullptr);
        CloseHandle(file);
    }
    return 0;
}
