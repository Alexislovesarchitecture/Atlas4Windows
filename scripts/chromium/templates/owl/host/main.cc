#include <windows.h>

#include <iostream>
#include <string>

int wmain(int argc, wchar_t* argv[]) {
  for (int i = 1; i < argc; ++i) {
    if (std::wstring(argv[i]) == L"--ping") {
      std::wcout << L"owl_host: pong" << std::endl;
      return 0;
    }
  }

  std::wcout << L"owl_host scaffold ready" << std::endl;
  Sleep(250);
  return 0;
}
