# План приведения Vulkan к OpenGL

Эталон: исходные проходы XRenderOGL и исходники вызываемых stock-шейдеров.
Цель: одинаковые материалы, освещение, прозрачность, тени, вода и эффекты
при одинаковых настройках игры. Стереопроекция OpenXR остаётся обязательной.
План активен; наличие сборки не означает завершение визуальной проверки.

## Порядок реализации

### Основной маршрут: последовательность кадра OpenGL

Работа ведётся по порядку выполнения эталонного кадра. Перечень семейств
шейдеров ниже служит учётом объёма, а не очередью отдельных визуальных исправлений.

1. BeginFrame / EF_StartEf: границы кадра и рекурсии, камеры, начальное состояние,
   очистки и время жизни данных до выполнения Vulkan-команд.
2. EF_EndEf3D: флаги кадра, splashes/client polygons, границы render-item списков.
3. EF_RenderPipeLine: PREPROCESS → STENCIL → GENERAL → UNSORTED → DISTSORT → LAST;
   условия пропуска и восстановления состояния после вложенного рендера.
4. EF_PipeLine: EF_PreRender(1), сортировка/preprocess, EF_PreRender(3),
   выбор объекта/материала/техники, flush, очистка состояния списка.
5. EF_Flush / EF_FlushHW: последовательность ambient/lightmap/light/shadow/fog
   проходов, vertex streams, параметры программ, samplers и GPU-состояния.
6. Draw: исходные формулы vertex/fragment программ, blend/depth/stencil/cull,
   alpha test, UV/форматы текстур, сохранение порядка от отправки до выполнения.
7. Завершение: screen/HDR/postprocess, удаление временных элементов,
   уменьшение уровня рекурсии, отправка и освобождение Vulkan-ресурсов.

Для каждого шага фиксируются функция OpenGL, функция Vulkan, входное и выходное
состояние, конкретное расхождение, изменение и статус проверки. Сборка,
GPU-замер и визуальная проверка обозначаются раздельно.

Первый контроль порядка: перенос воды в `VulkanFrameRenderer::sceneOrder`
является временной коррекцией отложенного списка, а не подтверждённым переносом
порядка OpenGL. Нужно установить источник ранней отправки и сопоставить все
границы списков/рекурсий; один замер passing samples не доказывает, что вода
сохраняется в итоговом изображении.

Пользователь подтвердил появление воды. Последовательный журнал первых
операций и оставшихся расхождений: `VULKAN_FRAME_SEQUENCE_AUDIT.md`.

### Учёт семейств и зависимостей

1. **Контракт программ и материалов** — явная классификация выбранных программ,
   параметров, samplers и feature masks; исключить незаметную подмену программы
   фиксированным комбайнером. Сохранить полный разбор поддержанных HW Layer.
2. **Terrain** — terrain-layer: overlay position, texgen, четвёртая степень
   distance fade, neutral-color blending, bump/diffuse/specular variants;
   terrain-shadow: projective shadow coordinates, alpha из shadow map,
   цвет объекта и terrain vertex alpha. Проверить low LOD и caustics.
3. **Растительность и модели** — CGRCPlants_Bump, bend/secondary color,
   ambient/diffuse/alpha; завершить варианты lighting templates,
   normal/gloss/specular, lightmap/directional lightmap и shadow receiver.
4. **Текстуры и render targets** — residency всех samplers, animated DSDT,
   native cube/volume samplers, специальные $-текстуры; жизненный цикл,
   layout transitions, deferred updates и восстановление состояния.
5. **Вода и отражения** — LowMed/Indoor/Outdoor/Sea/FFT/shore passes,
   mirrored camera $WaterMap, исходные projective matrices, clipping,
   reflection/refraction scheduling, дополнительные water variants.
6. **Preprocess OpenGL** — environment cube/light maps, SCANTEX/SCANSCR,
   refracted-object flush, screen capture, portals и rain maps.
7. **Полный набор программ** — перенос оставшихся Cg/NVParse семейств
   с явными Vulkan-модулями/вариантами и контрактами параметров.
   Парсер имени Cg-программы сам по себе не считается её реализацией.
8. **Fog/HDR/экранные эффекты и CRE** — volumetric fog, screen/HDR,
   heat vision, glare, оставшиеся particles/sprites/специальные элементы.
9. **Проверка соответствия** — перечень всех выбранных программ без
   неучтённых fallback, сборка всех зависимых библиотек, установка на Quest,
   сопоставление сцен Training и остальных уровней при одинаковых настройках.

## Условия завершения каждого прохода

- Сопоставлены vertex/fragment выражения, текстуры/форматы/UV и параметры.
- Сопоставлены alpha test, blend, depth, stencil, cull, clipping и порядок.
- Проход реально записывается и получает свои samplers, не fallback texture.
- Все shader variants и зависимые библиотеки входят в APK.
- Внешний вид подтверждён; непроверенные варианты отмечены явно.

## Текущая работа

- [x] Инвентаризация стадий и вызванных Training программ в отдельном аудите.
- [x] Исправление DSDT upload и водной fixed-combiner validation.
- [ ] Явные контракты для terrain-layer, terrain-shadow, bump-растительности.
- [ ] Перенос их исходных формул и ресурсов в Vulkan.
- [ ] Остальные этапы 3–9.

Ссылки на исходники и подтверждённые пробелы:
`VULKAN_PIPELINE_AUDIT_2026-09-30.md`. Этот план обновляется по результатам
реализации; ни один оставшийся пункт не считается выполненным заранее.
