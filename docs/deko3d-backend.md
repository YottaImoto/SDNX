# Бэкенд deko3d: спецификация

Продолжение §16.51 журнала: на ПК путь рендера целиком идёт через `GpuBackend` (Vulkan), на Switch нужен
второй бэкенд — deko3d — и презентер SDK. Здесь собрано всё, что нужно, чтобы его написать: модель
объектов, соответствие перечислений, последовательности вызовов и подводные камни, снятые с эталонного
рендера nfsmw (`D:\CODE\third_party\rexglue-sdk-nx\src\x360\deko`) и заголовка `deko3d.h`.

## Что уже есть

| Часть | Где | Состояние |
|---|---|---|
| Ядро deko3d: устройство, очередь, слоты, кольцо загрузок, куча образов, память кода, дескрипторы, показ | `nx/deko3d` (`rex::deko3d::Dispositivo/Nucleo/Presentacion`) | проверено на консоли тестовым NRO (§16.45, §16.46) |
| Пакет DKSH + `textures.txt` (привязки текстур по слотам Xenos) | `nx/shaders/tools/dk_build.py`, `nx/deko3d/include/sdnx/dk_package.h` | 1077/1077, консоль PASS |
| Интерфейс бэкенда | `me-sdk/include/rex/graphics/sdnx/gpu_backend.h` | этап 3, ступени A–D (§16.47–16.51) |
| Эталон маппинга состояний Xenos → deko3d | `rexglue-sdk-nx/src/x360/deko/dibujos.cpp` (строки 2140–2330) | работает в nfsmw |

## Модель объектов

`GpuBackend` считает, что у него есть образы, виды, буферы и шейдеры; на deko3d:

```
GpuImage   { DkImage image; DkMemBlock block; uint32_t offset; DkImageView view; ImageDesc desc; }
GpuView    { DkImageView view; }                 // формат/уровни/слои/свайзл
GpuBuffer  { DkMemBlock block; uint8_t* cpu; DkGpuAddr gpu; uint64_t size; BufferUsage usage; }
GpuShader  { DkShader shader; std::vector<sdnx::dk::PackageTexture> textures; }
```

* **Память образов** — `Nucleo::ReservarImagen` (куча образов, как слабы Vulkan-пула), `LiberarImagenLuego`
  освобождает после забора текущего слота. Кадровые образы (цели рендера, resolve) — из кучи; текстуры,
  которые живут долго, — из отдельных блоков (`DkMemBlock`), чтобы не занимать кучу.
* **Буферы**: `kUpload` — подвыделения кольца слота (`Nucleo::Reservar`, CPU-uncached, писать по порядку);
  `kGeometry`/`kReadback` — свои блоки (`DkMemBlockFlags_CpuUncached|GpuCached` и `CpuCached`); `kDeviceLocal`
  — блок без доступа CPU (`DkMemBlockFlags_GpuCached`).
* **Дескрипторы**: одна таблица образов (`Nucleo::DescriptoresImagen`) и одна сэмплеров
  (`DescriptoresSampler`); номер в таблице — это `DkResHandle`. Фронтенд держит отдельные индексы по кучам
  2D/3D/куб, поэтому хендл = `heap * 8192 + index` (в `ConfigNucleo::descriptores_imagen` нужно 3·8192 = 24576).
  Запись — только через `dkCmdBufPushData` в **pre-list** (`Nucleo::ComandosPrevios`), тогда все draw слота
  видят дескриптор: `dkImageDescriptorInitialize(&d, &view, false, false)` +
  `dkCmdBufPushData(previos, DescriptoresImagen() + handle*sizeof(d), &d, sizeof(d))`.
  Сэмплер: `dkSamplerDescriptorInitialize` в `DescriptoresSampler`.
* **Шейдеры**: `Nucleo::CargarShader(dkShader, dksh_bytes, name)`; после загрузки в середине кадра нужен
  `Nucleo::InvalidarCodigoSiHaceFalta()` (deko3d инвалидирует кэши только на `dkQueueFlush`).
* **Кадры**: `Nucleo::EmpezarTrabajo(slot)` / `TerminarTrabajo()`; `BeginFrame` = `EmpezarTrabajo`,
  `SubmitFrame` = `TerminarTrabajo`, `WaitIdle` = `EsperarGpu`.
* **Показ**: `Presentacion` (цепочка из 3 образов RGBA8) + `Adquirir/Presentar`; кадр фронт-буфера
  масштабируется в образ цепочки (compute-шейдер презента, как Vulkan-путь).

## Соответствие перечислений (Vulkan → deko3d)

Числа Vulkan в `DrawPacket` — нейтральные; deko3d **не** совпадает с ними порядком, кроме двух случаев.

| Что | Vulkan | deko3d | Правило |
|---|---|---|---|
| Сравнение (`DkCompareOp`) | NEVER=0 … ALWAYS=7 | None=0, Never=1 … Always=8 | `+1` |
| Трафарет (`DkStencilOp`) | KEEP=0 … DECREMENT_AND_WRAP=7 | Keep=1 … DecrWrap=8 | `+1` |
| Смешивание: операция (`DkBlendOp`) | ADD=0, SUBTRACT=1, REVERSE_SUBTRACT=2, MIN=3, MAX=4 | Add=1, Sub=2, RevSub=3, Min=4, Max=5 | `+1` |
| Смешивание: множитель (`DkBlendFactor`) | ZERO=0, ONE=1, SRC_COLOR=2, ONE_MINUS_SRC_COLOR=3, DST_COLOR=4, ONE_MINUS_DST_COLOR=5, SRC_ALPHA=6, ONE_MINUS_SRC_ALPHA=7, DST_ALPHA=8, ONE_MINUS_DST_ALPHA=9, CONSTANT_COLOR=10, ONE_MINUS_CONSTANT_COLOR=11, CONSTANT_ALPHA=12, ONE_MINUS_CONSTANT_ALPHA=13, SRC_ALPHA_SATURATE=14, SRC1_COLOR=15, … | Zero=1, One=2, SrcColor=3, InvSrcColor=4, SrcAlpha=5, InvSrcAlpha=6, DstAlpha=7, InvDstAlpha=8, DstColor=9, InvDstColor=10, SrcAlphaSaturate=11, Src1Color=16…, ConstColor=1\|0x20… | таблица (порядок другой!) |
| Примитив (`DkPrimitive`) | POINT_LIST=0, LINE_LIST=1, LINE_STRIP=2, TRIANGLE_LIST=3, TRIANGLE_STRIP=4, TRIANGLE_FAN=5 | Points=0, Lines=1, LineLoop=2, LineStrip=3, Triangles=4, TriangleStrip=5, TriangleFan=6 | таблица |
| Формат образа (`DkImageFormat`) | `VK_FORMAT_*` | `DkImageFormat_*` | таблица на используемый набор |
| Формат вершины (`DkVtxAttribSize`+`DkVtxAttribType`) | `VK_FORMAT_*` | `1x32/2x32/3x32/4x32`, `1x16/2x16/3x16/4x16`, `1x8…4x8`, `10_10_10_2`, `11_11_10` + `Float/Unorm/Snorm/Sint/Uint` | таблица |
| Фильтр | NEAREST=0 / LINEAR=1 | Nearest=1 / Linear=2 | `+1` |
| Адресация | REPEAT=0, MIRRORED=1, CLAMP_EDGE=2, CLAMP_BORDER=3 | Repeat=0, MirroredRepeat=1, ClampToEdge=2, ClampToBorder=3 | совпадает |

## Последовательность вызова для draw

Из эталона (`dibujos.cpp`, 2140–2330), в порядке записи; всё «только при изменении» — деко3д-состояние
живёт в командном буфере до смены.

1. `dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, {vs, ps, gs}, n)`
2. `dkCmdBufBindRasterizerState` — `cullMode` (биты 0/1 `cull`), `frontFace` (бит 2), `depthClampEnable`,
   `depthBiasEnableMask = DkPolygonFlag_All`
3. `dkCmdBufBindColorState` (маска включения смешивания по целям) + `dkCmdBufBindColorWriteState` (маски
   каналов из `color_mask`) + на каждую цель со смешиванием `dkCmdBufBindBlendState(cmd, i, &b)`
4. `dkCmdBufBindDepthStencilState` — глубина и трафарет из `stencil` (сырые биты `RB_DEPTHCONTROL`), как в
   эталоне: сдвиги 1/2/4/8/11/14/17/20/23/26/29
5. `dkCmdBufSetStencil(DkFace_Front/Back, mask, ref, funcMask)` — из `stencil_values[6]`
6. `dkCmdBufSetBlendConst`, `dkCmdBufSetDepthBias(constant, 0, slope)`, `dkCmdBufSetPrimitiveRestart`
7. `dkCmdBufSetViewports`, `dkCmdBufSetViewportSwizzles` (отрицательная высота → положительная + свайзл Y),
   `dkCmdBufSetScissors`
8. `dkCmdBufBindVtxAttribState` + `dkCmdBufBindVtxBufferState` (постоянные для входа; кэш по набору)
9. `dkCmdBufBindTextures(cmd, stage, 0, handles, n)` — хендлы собраны из `packet.shared` по `textures.txt`
   шейдера (слот Xenos → номер дескриптора в общем блоке) — для PS и, если есть, для VS
10. `dkCmdBufBindVtxBuffers(cmd, 0, extents, n)`, `dkCmdBufBindIdxBuffer(cmd, DkIdxFormat_Uint, addr)`
11. `dkCmdBufBindUniformBuffers(cmd, DkStage_Vertex/Fragment, 0..2, extents, n)` — блоки констант
    (`packet.constants[0..2]`), плюс `dkCmdBufBindStorageBuffers` для VS с `pull` (SSBO 3)
12. `dkCmdBufDrawIndexed(cmd, prim, index_count, 1, 0, vertex_offset, 0)` или `dkCmdBufDraw(...)`

Спец-константа (альфа-тест) на deko3d не существует: фронтенд пишет её в общий блок по байту 616, шейдер
читает оттуда (`SDNX_DEKO3D`, §16.46).

## Проходы рендера и переносы

* `BeginRendering(colors, count, depth, w, h)` → `dkCmdBufBindRenderTargets(cmd, views, count, depth_view)`;
  виды целей — `dkImageViewDefaults` + формат; `vkCmdBeginRendering`-аналога у deko3d нет, и clears делаются
  `dkCmdBufClearColor(targetId, mask, data)` / `dkCmdBufClearDepthStencil(clearDepth, depth, stencilMask, value)`
  **внутри** привязанных целей (это и есть `ClearAttachmentRect`).
* `CopyImage`/`BlitImage` — `dkCmdBufBlitImage(srcView, srcRect, dstView, dstRect, flags, factor)`;
  `CopyBufferToImage`/`CopyImageToBuffer` — соответствующие вызовы с `DkCopyBuf`; `CopyBuffer` —
  `dkCmdBufCopyBuffer(cmd, src, dst, size)`.
* `FullBarrier` — `dkCmdBufBarrier(cmd, DkBarrier_Full, 0)`; `AttachmentBarrier` — `DkBarrier_Graphics` с
  флагами инвалидации вложений (в эталоне — `dkCmdBufBarrier(cmd, DkBarrier_...`, сверить с §16.43).

## Показ кадра

`Present(slot, front, fw, fh, w, h, is_8bpc)`: `Presentacion::Adquirir()` → compute-шейдер презента
(`present.cs.hlsl`, уже есть для Vulkan) пишет в образ цепочки → `Presentar(index)`; затем
`TerminarTrabajo`. Образы цепочки — RGBA8, `DkImageFormat_RGBA8_Unorm`, 1280×720/1920×1080 из
`TamanoDePantalla`.

## Подводные камни (из эталона и §16.44–16.46)

* `dkImageLayoutMakerDefaults` + `ArreglarTamanoDeTesela(m, rows)` для block-linear 2D (иначе плитка уровня 0
  и мип 1 расходятся — §16.45).
* Отрицательная высота viewport в deko3d невозможна (unsigned): переворачивать свайзлом Y.
* `DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne` — строка 0 сверху, z 0..1 (как Direct3D).
* Инвалидация кэшей после загрузки кода в середине кадра.
* Ошибка deko3d в релизе завершает процесс: перед каждым созданием — `VaciarRegistro()`.
* Динамических индексов компоненты в UBO нет (баг uam, §16.44) — если понадобится, писать константы
  блоками.

## Порядок работ

1. **Каркас** (`deko3d_backend.h/.cpp`): объекты, кадры, буферы, образы, виды, дескрипторы, очистки,
   копии, барьеры, шейдеры (DKSH), `Present`. Проверка — сборка NRO и тестовый кадр с очисткой.
2. **Draw**: маппинг состояний (таблицы выше), `BeginRendering/Draw/EndRendering`, привязки текстур по
   `textures.txt`. Проверка — кадр меню на консоли против стенда.
3. **Показ кадра**: `Presentacion` на оконечном `NWindow`; бэкенд владеет цепочкой и показывает кадр сам
   (фронтенд всегда вызывает `backend_->Present`). Порядок: блит фронта в образ цепочки → `TerminarTrabajo`
   → `Presentar`. Презентер SDK и Vulkan-провайдер на консоли не создаются (`SDNX_DEKO3D=1`).
4. Замеры: мкс CPU на draw на консоли (ориентир из §16.45: 153–184 нс на пустой draw; в игре — цель
   ≤ 8 мкс), fps.
