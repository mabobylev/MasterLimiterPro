# MasterLimiterPro

Linux/JUCE VST3. Общаться на русском. Перед правками читать README.md,
docs/PLAN.md и актуальные решения docs/DECISIONS.md; проверять git status.

## Структура и работа

Source/ — DSP и GUI, Tests/ProcessorTests.cpp — тесты процессора/истории/превью.
JUCE/ — локальная зависимость вне Git, закреплена в JUCE_REVISION; не удалять
её при уборке. build/ — текущая сборка, полностью игнорируется Git.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target MasterLimiterTests -j2
MASTERLIMITER_PREVIEWS=1 ctest --test-dir build --output-on-failure
cmake --build build --target MasterLimiterPro_VST3 -j2
git diff --check
```

Установка после успешной сборки в ~/.vst3/Master Limiter Pro.vst3 разрешена
пользователем автоматически; проверять SHA-256 установленной копии.
Для CI/изолированной проверки задать MASTERLIMITER_COPY_PLUGIN_AFTER_BUILD=OFF.
DAW и прослушивание остаются за пользователем. Различать автотесты и приёмку.

## Инварианты

Сохранять старые ID/индексы/диапазоны параметров и миграцию состояний.
Не добавлять аллокации/блокировки/IO в аудио. История — signed min/max L/R
и GR на одной sample-шкале; передавать согласованные snapshots.
True Peak проверять на окончательном выходе независимым интерполятором;
данные тесты не являются сертификацией BS.1770. Не менять допуски ради PASS.
GUI проверять в минимальном и увеличенном окне. Не затрагивать чужие плагины.

## Skill

Версионируемый источник общего навыка: tools/skills/juce-audio-plugin-dev/.
Установленная копия: /home/bma/.codex/skills/juce-audio-plugin-dev/.
При согласованном обновлении навыка синхронизировать копии и валидировать
quick_validate.py из системного skill-creator. Специфику этого проекта
держать в docs/, общие проверенные уроки — в skill/references/.
Не коммитить build, JUCE, токены, локальные агентские настройки и архивы.
