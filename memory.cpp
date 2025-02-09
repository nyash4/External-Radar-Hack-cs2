#include "memory.h"


namespace VARS {
	DWORD GetProcess(const wchar_t* Target) {
		HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

		if (snapshotHandle == INVALID_HANDLE_VALUE) {
			return NULL;
		}

		PROCESSENTRY32W processEntry = { };
		processEntry.dwSize = sizeof(PROCESSENTRY32W);

		if (Process32FirstW(snapshotHandle, &processEntry)) {

			do {
				if (_wcsicmp(processEntry.szExeFile, Target) == 0) {
					CloseHandle(snapshotHandle);
					return processEntry.th32ProcessID;
				}
			} while (Process32NextW(snapshotHandle, &processEntry));
		}

		CloseHandle(snapshotHandle);
		return NULL;
	}

	uintptr_t GetModuleBaseAddress(DWORD processId, const wchar_t* ModuleTarget) {

		HANDLE snapshotHandle = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);

		if (snapshotHandle == INVALID_HANDLE_VALUE) {
			return NULL;
		}

		MODULEENTRY32W moduleEntry = { };
		moduleEntry.dwSize = sizeof(MODULEENTRY32W);

		if (Module32FirstW(snapshotHandle, &moduleEntry)) {

			do {
				if (_wcsicmp(moduleEntry.szModule, ModuleTarget) == 0) {
					CloseHandle(snapshotHandle);
					return reinterpret_cast<uintptr_t>(moduleEntry.modBaseAddr);
				}
			} while (Module32NextW(snapshotHandle, &moduleEntry));
		}

		CloseHandle(snapshotHandle);
		return NULL;
	}

	std::string memReadString(uintptr_t pointerStatic, size_t maxLength) {
		char buffer[256] = { 0 };
		ReadProcessMemory(VARS::processHandle, (LPCVOID)pointerStatic, buffer, maxLength - 1, NULL);
		return std::string(buffer);
	}

	struct RemoteStdString {
		uintptr_t dataPtr;  // Указатель на строку в памяти процесса
		size_t size;
		size_t capacity;
	};

	std::string memReadStdString(uintptr_t pointerStatic) {
		RemoteStdString remoteStr;
		ReadProcessMemory(VARS::processHandle, (LPCVOID)pointerStatic, &remoteStr, sizeof(remoteStr), NULL);

		if (remoteStr.size == 0 || remoteStr.dataPtr == 0)
			return "";

		std::vector<char> buffer(remoteStr.size + 1);
		ReadProcessMemory(VARS::processHandle, (LPCVOID)remoteStr.dataPtr, buffer.data(), remoteStr.size, NULL);
		buffer[remoteStr.size] = '\0';

		return std::string(buffer.data());
	}

	DWORD processId = GetProcess(L"cs2.exe");
	uintptr_t baseAddress = GetModuleBaseAddress(processId, L"client.dll");
	HANDLE processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);

}