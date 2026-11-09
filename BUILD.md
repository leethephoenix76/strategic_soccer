# Сборка Strategic Soccer в VS Code (Windows)

Нужны расширение **C/C++** (ms-vscode.cpptools) и компилятор MSVC: «Build Tools for Visual Studio» с нагрузкой **Desktop development with C++**.

```
winget install --id Microsoft.VisualStudio.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

`build_msvc.bat` сам находит компилятор через `vswhere`, поэтому запускать VS Code из Developer Prompt не нужно.

| Действие | Как |
| --- | --- |
| Собрать игру, `ssbot` и тесты движка | `Ctrl+Shift+B` (задача **Build**) |
| Прогнать тесты движка | `Ctrl+Shift+P` → *Tasks: Run Test Task* (задача **Run engine tests**) |
| Собрать тесты бота | `build_msvc.bat --bot-tests` |
| Запустить с отладчиком | `F5` → **Debug game** (или **Debug engine tests**) |

Результат: `build\soccer.exe` — игра, `build\ssbot.exe` — консольная утилита, `build\engine_tests.exe` — тесты движка. `build\bot_tests.exe` собирается только с `--bot-tests`; сборка сама тесты не запускает. Вывод компилятора сохраняется в `build\build.log`.

Устройство кода описано в [docs/architecture.md](docs/architecture.md), правила игры — в [docs/game_model.md](docs/game_model.md).
