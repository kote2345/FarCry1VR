# SCANCM / SCANLCM: исходный контракт OpenGL

Перенос ещё не завершён: вызов NULL `ScanEnvironmentCube` не создаёт capture.
Документ фиксирует требования к следующей реализации, без статуса готовности.

## Порядок

1. Preprocess выбирает RE, позицию объекта и временный ignore object.
   Capture пропускается при draw-to-texture и нулевой позиции. SCANLCM
   дополнительно требует FOB_ENVLIGHTING и r_envlighting.
2. Общий ShaderCore выбирает/обновляет environment cache с учётом позиции,
   расстояния до камеры, времени и in-progress/ready состояния.
3. ScanEnvironmentCube выбирает 64/128/256/512 по r_envcmresolution,
   уменьшает размер до размещения 3×2 сторон в viewport; <=8 пропускается.
4. Неготовый cube или envcmwrite обновляет все шесть сторон. Готовый cube
   обновляет сторону m_MaskReady. In-progress предотвращает рекурсию.
5. Сохраняются viewport/fog/camera; устанавливается draw-to-texture.
   Capture position = RE center(current object) + environment CamPos.
6. DrawCubeSide строит исходную камеру каждой стороны и запускает DrawLowDetail
   с исходными RendFlags. Обычные opaque/material/light/fog passes должны
   исполняться полноценно в offscreen target, без OpenXR eye reprojection.
7. Стороны копируются в настоящий cubemap. После шестой стороны m_bReady=true,
   m_MaskReady=0; viewport/fog/draw-to-texture и ignore object восстанавливаются.
8. Материальные программы используют cube sampler и исходные reflection vectors.
   Наличие загруженного 2D atlas не доказывает соответствие samplerCube.

## Уже перенесённые предпосылки

- Сбор/sort preprocess по SPRID с сохранением source object, RE и resources.
- Capture eligibility для SCANCM/SCANLCM.
- Ignore-source правило EF_ObjectChange: совпадение позиции, исключение для sky.

## Требуемая следующая реализация

- Native six-layer cube image/view, samplerCube descriptors и lifetime.
- Offscreen scene target, собственная projection/view и scope команд.
- Полноценный material-pass recorder для capture, не shadow-only recorder.
- Camera/fog/light/viewport/state restoration при вложенном рендере.
- Общий ShaderCore cache с фактическим GPU completion/ready контрактом.
- Исходные cube-sampling программы и проверка всех шести ориентаций.

Эталон: GLRendPipeline.cpp EF_Preprocess; GLTextures.cpp
ScanEnvironmentCube/DrawCubeSide; Common/Shaders/ShaderCore.cpp
mfFindSuitableEnvCMap/mfFindSuitableEnvLCMap.
# GPU resource progress

`VulkanResourceManager::CreateCubeColorTarget` now allocates a cube-compatible
six-layer image, a CUBE sampling view and six independent 2D attachment views.
Partial allocation failures release all created views, image and memory.
The resource starts in UNDEFINED layout; the capture recorder must transition
each face before rendering and make the completed faces shader-readable.

Native libraries and the release APK build successfully. The allocator is not
yet connected to environment capture or material descriptors; this change alone
does not implement reflections or change the displayed scene.
