# AGENTS.md — правила моддинга GTA: Vice City

Рабочая директория: `D:\Games\Grand Theft Auto Vice City`.
Эти правила обязательны. Цель — минимальный расход токенов и ноль повреждений оригинальных файлов.

## Установленные инструменты (проверено)

| Инструмент | Путь / вызов | Назначение |
|---|---|---|
| ripgrep 15.2 | `rg` (через WinGet Links) | точечный поиск в конфигах и скриптах |
| Blender 5.2.2 LTS | `& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"` | headless-работа с .dff/.txd/.col |
| DragonFF (ext `dragonff`) | установлен в Blender, `bl_ext.user_default.dragonff` | импорт/экспорт DFF, TXD, COL, IPL |
| FFmpeg 9.0.2 | `ffmpeg` | конвертация аудио для радио/SFX |
| Open Limit Adjuster v2.0.6 | `III.VC.SA.LimitAdjuster.asi/.ini` в корне | снятие лимитов |
| ViceCityToolset v5.0.1 | `_tools\ViceCityToolset\ViceCityToolset.exe` | IMG/COL (GUI) |
| rwexporter v1.0 | `_tools\rwexporter\rwexporter.exe` | CLI-конвертация версий DFF/TXD |
| plugin-sdk (master) | `_tools\plugin-sdk\` (premake: `_tools\plugin-sdk\tools\premake\premake5.lua`, premake5.exe в комплекте) | сборка нативных .asi |
| DragonFF исходники | `_tools\DragonFF\` | python-модуль `gtaLib` (dff, txd, col) |
| Sanny Builder 4 | `& "C:\Program Files\Sanny Builder\sanny.exe"` | компиляция CLEO-скриптов (CLI подтверждён) |
| MSVC 14.44 + SDK 10.0.26100 | `cl.exe` через vcvars64.bat (BuildTools 2022) | сборка .asi (проверено: компиляция+линковка+запуск) |

## Правило 1. Никогда не читать большие файлы целиком

Запрещено полностью читать: `*.ipl`, `handling.cfg`, `default.ide`, `ped.dat`, `weapon.dat`,
`gta3.dir`, `roads.lod`, а также любые `data/*.dat|cfg|ide` больше ~200 строк.

Вместо этого:

```powershell
# найти строку и контекст ±2
rg -n -C 2 "infernus" data\handling.cfg
rg -n "coleman" data\peds.ide

# точечная замена одной строки по якорю
$f='data\handling.cfg'; (Get-Content $f) -replace '(?<=^INFERNUS\s).*', ' <новый хвост> ' | Set-Content $f -Encoding ascii
```

```python
# python: замена строго по якорю строки
import io, re
p = r"data\handling.cfg"
s = io.open(p, encoding="ascii", errors="ignore").read()
s = re.sub(r"(?m)^(INFERNUS\s+\S+\s+\S+)", r"\1", s, count=1)
io.open(p, "w", encoding="ascii", newline="\n").write(s)
```

Правила:
- сначала `rg -n`, потом замена по уникальному якорю, потом `rg` для проверки;
- ASCII-кодировка, CRLF сохранять; не менять порядок секций;
- .ide/.ipl — только добавление/правка нужных строк, не реформат.

## Правило 2. Куда складывать файлы мода

- Всё содержимое мода (конфиги с сохранением структуры `data/`, `models/`, `txd/`,
  `.dff`, `.txd`, `.col`, аудио) → строго `modloader\ai_mod\`.
  Пример: `modloader\ai_mod\models\newcar.dff`, `modloader\ai_mod\data\handling.cfg`.
- Скомпилированные CLEO-скрипты (`.cs`) → строго `CLEO\`.
- Ничего не менять напрямую в `models\gta3.img`, `txd\`, `data\`, если задача
  явно не требует этого (тогда это отдельное осознанное действие).

## Правило 3. 3D-модели: Blender headless + DragonFF

Blender всегда запускать в фоне:

```powershell
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --python script.py
```

Проверенные операторы DragonFF (расширение включено в user-prefs):

```python
bpy.ops.import_scene.dff(filepath="X.dff")                       # импорт модели (+ арматура, материалы)
bpy.ops.export_dff.scene(filepath="X.dff", directory=..., only_selected=False)
bpy.ops.scene.dragonff_txd_export(filepath="X.txd", directory=..., only_used_textures=False)  # TXD v0x36003 (VC)
bpy.ops.export_col.scene(filepath="X.col", directory=..., only_selected=False)                # коллизия
bpy.ops.scene.dragonff_map_import(filepath="X.ipl")              # карта/цимсы
```

Упаковка PNG → TXD (проверенный рецепт):

```python
import bpy
img = bpy.data.images.load(r"path\texture.png")
bpy.ops.scene.dragonff_txd_export(filepath=r"out.txd", directory=r"path", only_used_textures=False)
```

Работа с gtaLib без GUI:

```python
import sys; sys.path.append(r"D:\Games\Grand Theft Auto Vice City\_tools\DragonFF")
from gtaLib import col   # чтение/правка .col как python-объектов
from gtaLib import dff, txd
```

## Правило 4. Лимиты и границы мира

- Файл `III.VC.SA.LimitAdjuster.ini` в корне. Лимиты по умолчанию `unlimited`
  (растут по необходимости). Уменьшать/фиксировать только при реальной потребности:
  `Objects = 14000`, `Peds = 140` и т.п. `0` = лимит игры не тронут.
- Границы мира: `data\*` (water/ipl cull zones) + правка `.col` при расширении.
- Диагностика перелимитов: лог `III.VC.SA.LimitAdjuster.log` в корне — читать
  хвост через `Get-Content -Tail 40`, не целиком.

## Извлечение из IMG (без GUI)

```python
import struct
# gta3.dir: записи по 32 байта: u32 offset, u32 size (в секторах 2048), имя[24]
# извлечение: img.seek(off*2048); img.read(size*2048)
```

Либо ViceCityToolset вручную. Перед правкой существующих .dff/.txd — извлечь
оригинал из `models\gta3.img` во временную папку, редактировать копию, класть
результат в `modloader\ai_mod\`.

## Аудио (ffmpeg)

- Радио-стримы: `ffmpeg -i in.wav -c:a libvorbis -qscale:a 6 out.ogg` →
  `modloader\ai_mod\audio\streams\...`.
- SFX-пак: mono 16-bit PCM: `ffmpeg -i in.mp3 -ac 1 -ar 32000 -c:a pcm_s16le out.wav`.
- Не трогать оригинальные `Audio\SFX.PAK` / `Audio\Streams` без явной задачи.

## Сборка нативных плагинов (plugin-sdk)

- MSVC установлен (BuildTools 2022, cl.exe 14.44.35207, SDK 10.0.26100). Проверено.
- Окружение для cl.exe: `"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"`.
- Генерация проекта: `premake5 vs2022` (exe в `_tools\plugin-sdk\tools\premake\`),
  для VC — проект `plugin_vc`. Итоговый `.asi` тестировать в `scripts\`/корне.

## CLEO-скрипты

В игре CLEO **v2.0.0.6 (классический порт)** — понимает только сырой SCM-байткод.

Проверенная команда компиляции (тест прошёл: `00 00 93 0A` для NOP+terminate):

```powershell
& "C:\Program Files\Sanny Builder\sanny.exe" --mode vc -o Compiler::AddExtraInfo 0 --compile src.txt out.cs
```

- ОБЯЗАТЕЛЬНО `-o Compiler::AddExtraInfo 0`: без этого SB4 оборачивает .cs в
  SBF-контейнер (`FLAG...__SBFTR` + исходник), который классический CLEO не выполнит.
- Режим строго `--mode vc` (не `vc_sbl`).
- Источники: `modloader\ai_mod\_src\*.txt` с `{$CLEO .cs}` в начале; результат — в `CLEO\`.
- Ошибки компиляции смотреть в `compile.log` рядом с выводом.

## Анти-мусор

- Не оставлять временные файлы в корне игры; `D:\Android\Temp\opencode` — для мусора.
- Не создавать дубликаты (например, вторые копии .asi).
- Логи читать хвостом (`-Tail`), отчёты — кратко.
