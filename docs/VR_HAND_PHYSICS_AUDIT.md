# Разбор физики рук — 2026-10-05

## Требуемое поведение

Контроллер задаёт желаемую позу. Физическая кисть удерживает предмет и передаёт
нагрузку через руку телу. Лёгкий предмет устойчиво следует кисти, тяжёлый создаёт
нагрузку. При достижении длины руки неподвижный предмет ограничивает отход игрока.
Масса предмета сама по себе не оправдывает самопроизвольное вращение кисти.

Этот документ — анализ исходников и уже разрешённых пользователем журналов.
Он не подтверждает стабильность последней установленной сборки. В этом разборе
новый APK не создавался и новые журналы со шлема не запрашивались.

## Текущий путь данных

1. `CryVR.cpp` получает OpenXR grip pose и squeeze input.
2. `CrySystem/SystemRender.cpp::GetVRControllerTransform` переводит pose из
   OpenXR в мир игры через опорную ориентацию головы и игровую камеру. Мировая
   pose включает перемещение/поворот игрового тела, а не только движение руки.
3. `VRPhysicalWeapons.cpp::Update` считывает pose, вычисляет скорость разностью
   кадров, выбирает предмет и создаёт линейное и угловое соединения.
4. `XPlayer.cpp::UpdateVRBodyPhysics` временно строит IK-позу скелета и вызывает
   `VRBodyPhysics.cpp::Resolve`, затем восстанавливает матрицы анимации.
5. `Resolve` управляет девятью независимыми rigid bodies: торс, две ключицы,
   два плеча, два предплечья, две кисти. Все получают отдельные мировые PD-цели.
6. При хвате добавляется цепь точечных суставов до торса. Предмет связан с кистью
   шестью ограниченными степенями свободы. Решатель применяет ограничения в
   шагах CryPhysics.
7. Root игрока остаётся `PE_LIVING`. Ограничитель ходьбы и дополнительные
   импульсы пытаются связать его с нагрузкой рук.
8. Отрисовка опять решает IK, адаптирует цепи к физическим сегментам и закрепляет
   видимую кисть на точке предмета.

## Подтверждённые проблемы конструкции

### 1. Нет анатомических суставов

`UpdateArmConstraints` использует только `pe_action_add_constraint.pt`.
Локоть, запястье и плечо — свободные шарниры без ограничений сгибания/скручивания.
Отдельные мировые приводы пытаются компенсировать отсутствующие ограничения.
Это не модель человеческой руки: запястье не имеет анатомического диапазона,
а локоть допускает движения, которые человек совершить не может.

### 2. Приводы конкурируют в соединённой цепи

Торс, ключица, плечо, предплечье и кисть получают независимые линейные импульсы
к своим мировым центрам. Суставы при этом требуют совпадения локальных точек.
Цели всей цепи не решаются как единая задача с учётом нагрузки/контактов.
У кисти есть дополнительная цель из захвата, у предплечья — цель из IK.
Несогласованность создаёт внутренние силы и моменты, даже если предмет лёгкий.
По журналам нельзя выделить долю каждого привода: их усилия пока не записаны.

### 3. Не согласованы частота управления и шаг физики

`Resolve` ограничивает один импульс по `GetFrameID()` и игровому frame time.
CryPhysics может ограничить world step и разбить его на несколько substeps.
`iApplyTime=1` складывает импульс в `m_Pext[0]/m_Lext[0]`; `CRigidEntity::Step`
целиком принимает его в первом подходящем шаге перед интегрированием.
Привод не пересчитывается по ошибке и скорости после каждого substep.
Clamp времени до 0.05 дополнительно меняет оценку скорости относительно времени
между двумя измерениями. Простое изменение коэффициентов не устраняет это.

### 4. Создание суставов может сразу породить коррекцию

Точки суставов задаются из желаемого скелета `targetA`, а локальные точки
сохраняются относительно фактических физических тел. Их текущие положения
могут отставать от скелета. Не проверяется, что полученные локальные точки
соответствуют анатомическим концам сегментов. Захват также допускает разные
начальные точки предмета и кисти. Нужны постоянные локальные anatomical frames,
согласованная начальная сборка и контролируемое включение привода.

### 5. Массы/инерции рук и приводы не образуют общей модели

Масса каждой кисти — 3.5 кг, каждого другого сегмента руки — 2 кг, торса — 15 кг;
гравитация сегментов отключена. Для обычного углового привода используется
приближённая скалярная инерция. Для кисти с предметом — сумма тензоров двух тел.
Это не эффективная инерция всей соединённой цепи. Последняя правка убрала
усиление через расстояние между COM, но сама проблема согласования осталась.
Пределы 350/600/900 Н и 40 Нм не выводятся из общей модели мышечной силы.

### 6. Во время хвата отключаются контакты руки

При `held` у геометрий сегментов этой руки `flagsCollider` становится нулём.
Предмет сохраняет свои контакты, но кисть, предплечье и плечо перестают
взаимодействовать с окружением. Это нарушает требование физических рук.
Следует исключать конкретные нежелательные пары (самоколлизии и контакт с
удерживаемой геометрией), сохраняя столкновения со стенами/окружением.

### 7. Реакция на тело не является реакцией всей цепи

Торс руки не соединён с `PE_LIVING` физическим суставом. Сейчас root получает
только горизонтальную положительную составляющую команды привода кисти и
дополнительные импульсы ограничителя ходьбы. Команда привода не равна принятой
решателем внешней нагрузке. Нагрузка проходит двумя разными приближёнными
путями; нет общего учёта импульса. Инерционная часть ограничителя не ограничена
пределом 350 Н, в отличие от части тяги стиком.

`CLivingEntity::Action(pe_action_impulse)` умножает импульс на `massinv*0.5`.
Расчёт торможения в ограничителе этого не учитывает. Поэтому равные переданные
импульсы предмету и living root не дают ожидаемой реакции и торможения игрока.

### 8. Отрисовка скрывает расхождение физического контакта

`GetPropHandTarget` берёт вращение физической кисти, но вычисляет её видимую
позицию от точки предмета. Финальный `ApplyVRArmIK(...physicalContact=true)`
ставит видимое запястье туда независимо от обычного ограничения длины руки.
Внешне хват может выглядеть замкнутым, хотя физические точки уже разошлись.
`Resolve` с нулевым временем также способен создавать тела/суставы и менять
маски коллизий — отрисовка пока не является чистым чтением состояния.

### 9. Управление вращением и диагностика неполны

Ориентация не удерживаемого сегмента определяется одним вектором вдоль капсулы:
так нельзя определить twist. У удерживаемой кисти теперь есть полная pose, но
остальные сегменты продолжают использовать эту неоднозначную ориентацию.
Скорость контроллера рассчитывается в мировом пространстве и включает поворот
игрового тела. Эти компоненты не выделены отдельно для feed-forward.

Записываемый `angle` — ошибка **предмет относительно кисти**, не ошибка
**кисть относительно цели контроллера**. Малая первая величина не означает,
что рука правильно следует контроллеру. Нужны обе ошибки, actual/target angular
velocity, команды/принятые моменты, frame dt/substep dt и имя модели.

### 10. Дополнительный дефект API решателя

`RegisterConstraint` возвращает -1 при исчерпании маски. `Action(add_constraint)`
не проверяет этот результат до обращения к `m_pConstraints[i]`. Это отдельный
риск повреждения памяти; текущие журналы не доказывают, что он уже возникал.

## Что уже подтверждали разрешённые журналы

- `5771` пользователь определил как коробку: её масса была 12.5 кг.
- `5738` в проверке банки: масса 1.348 кг; разрыв точек до 0.20448 м,
  ошибка относительной ориентации до 0.6170 рад, controller lag до 0.402 м.
- Другой захват начинался с разрыва до 0.96333 м.
- Малые относительные ошибки после стабилизации не оправдывают первоначальный
  срыв или раскачивание всей связки относительно контроллера.
- Новая сборка с массами 0.35/0.5 для выбранных моделей ещё не подтверждена
  новой проверкой. Имена моделей этих entity пока не записаны, поэтому нельзя
  утверждать, что правило массы применилось к конкретному предмету.

## Последовательность переделки

1. Разделить приём tracking, физическое управление и визуализацию. Отрисовка
   только читает результат; создание/удаление тел и суставов — в update.
2. Ввести единые локальные frames кисти/запястья/локтя/плеча, разумные массы и
   тензоры инерции, ограничения суставов. Согласовать тела до включения хватов.
3. Перенести приводы в physics substep: ограниченные силы/моменты и стабильное
   управление с учётом фактической скорости. Рука управляется связанной цепью,
   а не набором конкурирующих мировых springs.
4. Сохранить 6DOF предмет–ладонь, обеспечить полную позу кисти и проверить
   прохождение линейной/угловой нагрузки через решатель. Исключить двойное
   добавление нагрузок и компенсацию дрейфа через коэффициенты массы.
5. Вернуть коллизии рук с миром с корректным исключением отдельных пар.
6. Связать ограничение root/reach с фактической реакцией решателя и явно
   определить семантику PE_LIVING. Стик, поворот тела и room-scale — отдельные
   источники движения; управление камерой не должно скрывать отрыв тела.
7. Добавить измерения цели/кисти/предмета и подтверждать этапы на шлеме:
   неподвижное удержание, вращение, ходьба, упор в стену, две руки, тяжёлый груз.
   Чтение журналов и запуск игры — только по команде пользователя.

Следующий патч должен менять механизм управления, а не снова подбирать массу
или коэффициенты поверх текущей несогласованной цепи.
# Checkpoint: substep tracking drive, 2026-10-05

## Follow-up: bound the player/load reaction and joint error recovery

The anatomical-frame build failed the user's test: the log starts with
sub-millimetre grip error, then jumps to 40.28457 m separation. Reported hand
motor forces/torques remain within 350 N/40 Nm. The initial constraint failure
is not established by those samples. Source review confirmed a separate
amplification path in `LimitNPCGripMovement`: unbounded playerMass*coast impulse,
negative allowed speeds becoming inward locomotion commands, and positional
joint error recovery at unbounded drift*10 velocity.

Changes: nonnegative allowed outward walking speed; reduced pair mass using
PE_LIVING's half-impulse response; cap the complete reaction at 350 N per hand;
skip additional reaction across a grip separated by more than 10 cm without
releasing grip; apply player impulse directly instead of through the living
move/jump command; limit positional recovery speed of VR joints to 2 m/s.

Verification adds 20000 randomized movement/reaction cases, light/heavy-load
momentum examples, source guards, existing frame/drive checks and Android build.
This fixes confirmed launch amplification bugs, but headset testing is still
required to establish that the initial full-chain constraint failure is gone.


## Follow-up: anatomical frames instead of capture-lag frames

Latest user feedback still showed large wrist/controller angular error and
intermittent grip separation with the bounded solver motor. Code review found
two reference-frame inconsistencies: controller-to-hand calibration was captured
from the displaced physical hand, and arm joints captured a shared tracked world
point against displaced segment poses. Both preserve the instantaneous tracking
error as a permanent local offset and can make motor targets incompatible with
anatomical joints during rotation.

Fixed: capture the hand drive reference from the raw tracked IK pose; drive its
center using that controller-local anatomical offset; attach arm segments at
their actual local anatomical endpoints. Grip/object relative rotation remains
captured from actual physical poses, independently of the tracking reference.
Rendering recovers the wrist from the same physical hand/reference transform.
This avoids substituting object-anchor pinning for the physical wrist position.

`pe_status_vr_drive` reports the last solved force/torque, target pose and step age;
`VRGripDrive` records them beside grip errors. The build marker is
`frames=anatomical-v2`. Additional checks cover 1500 arbitrary rotations and
capture-pose disturbances. They do not prove the full hand chain is stable in
CryPhysics; the user must validate this checkpoint on the headset.

References inspected:
- https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/Joints.html
  (local attachment frames, implicit drives, convergence and inertia)
- https://github.com/Barliesque/HandsOnVR
  (separate controller target and physical hand, physical mass-based grabs)
- https://github.com/jorgejgnz/MinimalHandPhysics
  (alternative physical hand interaction approach; not copied into this patch)


## Follow-up: load-aware solver motors

The headset feedback confirmed correct masses (can 0.35 kg, box 0.5 kg), but the
box's physical wrist/controller error reached 2.2906 rad while the grip's own
relative rotation error remained small. Substep impulses scaled by the isolated
hand inertia were insufficient for the coupled assembly.

The tracking drive now registers two bounded compliant motor contacts in the
same iterative solve as grip, arm constraints and collisions. Spring gains have
physical units (6000 N/m and 300 Nm/rad at the nominal setting), independent of
the isolated hand mass/inertia. Accumulated impulses are clamped to force*dt and
torque*dt. Compliance is included in the residual on every iteration. Existing
CG shortcuts are skipped for islands with these motors, since they assume hard
unbounded constraints. The energy guard retains its existing limits and adds
only the measured kinetic-energy work of accepted motor impulses.

Held hands retain normal drive strength during contact. An additional delayed
angular feed-forward filter was removed. Grip creation waits until the actual
physical hand is within 35 cm of the acquisition point, preventing a new remote
attachment when a previous disturbance left a hand far away; this distance check
does not break an established grip.

Validation: Android native build, scalar coupled-load PGS checks for five step
sizes (rotation, translation, immovable obstacle, accumulated force cap), frame
checks and source review. Numerical checks do not execute the full CryPhysics
solver. Headset validation, two-hand contact stability and performance remain
necessary before declaring this checkpoint accepted.

Implemented after the audit:

- `pe_action_vr_tracking` submits a pose/velocity target from game update.
  `CRigidEntity::Step` applies a bounded implicit PD impulse on every physics
  substep, using the actual body inertia rather than applying the load's inertia
  to the hand before constraint solving. Stale targets expire after 250 ms.
- Hand mass is 0.6 kg. Full orientation grip and arm attachment constraints remain.
- Acquiring a grip uses coincident world anchors and preserves the initial pose.
- A rigid body constrained to a VR body switches from the penalty solver to the
  full coupled solver. This remains enabled after release, including two-hand
  release, so a second hand cannot accidentally restore the incompatible mode.
- Render-only calls no longer create bodies, submit drives, or create arm joints.
  Held-object collision filtering retains collisions with the environment.
- Grip diagnostics additionally measure hand/controller orientation error and
  report whether the held body has the simple solver flag.

Checks: native Android build; numerical bounded-drive/convergence checks at seven
step sizes (77 cases); 1000 randomized grip-frame/anchor checks; source guards and
`git diff --check`. These do not execute the CryPhysics contact solver and do not
prove stability on the headset. Anatomical angular limits and a complete player
root reaction redesign are still outstanding. The current checkpoint addresses
the tracking/grip faults and requires the user's headset test before proceeding.


## 2026-10-06: carried rotation and owner contact conflicts

The previous headset feedback still showed large controller/wrist rotation error
and 40 Nm saturation, while the welded grip itself mostly stayed within millimetres.
This does not alone prove the cause of the residual wrist instability.

Implemented:

- The bounded angular wrist motor for a rigid held prop is now registered on the
  load side of its existing fixed angular joint. The target is mapped through
  both captured joint frames and the hand principal-inertia frame. There is one
  angular motor per hand, no extra object servo, and the 40 Nm accumulated limit
  remains. This avoids the small hand inertia/load inertia iterative bottleneck.
  Linear drives and the physical six-axis grip remain in the coupled solve.
- Rigid held props exclude only the carrier capsule and that carrier's VR body
  parts. The exclusion is symmetric, includes sweep tests and living ground
  selection/synchronization, purges old contact masks, and leaves all joint
  constraints intact. Two hand bits preserve exclusion until the last release.
  Props still collide with the environment and other characters.
- Removed the old controller-gap locomotion limiter. Anatomical arm reach,
  bounded reduced-mass pull and direct living velocity reaction remain.
- Build marker: `carrier=pair-filter-v1 angular=load-side-v1`.
  Drive diagnostics now report `angularBody`, so a later requested log can confirm
  that a held rigid prop actually receives the mapped angular drive.

Checks: existing tracking, loaded motor, grip frame and reaction scripts; new
`research/check_vr_carried_rotation.py` with 5000 randomized frame conversions,
rotation reversals and settling at four dt values, three inertias, three solve
budgets, force limits and owner/two-hand lifecycle checks. These are reduced
numerical models and source guards, not execution of the full native solver.
The user's next headset test must validate wrist response, world contact,
backward locomotion, immovable loads and two-hand release.


## 2026-10-06: bounded wrist angular velocity servo

Headset feedback rejected the load-side spring checkpoint: 0.5 kg box,
161-degree peak controller/wrist error, 52/87 recorded samples at 40 Nm.
The `angularBody` diagnostic confirmed that the new load-side code ran; this
was not a stale APK. The diagnostics alone did not establish the root cause.

Held wrists now opt into a bounded angular velocity servo instead of the
300 Nm/rad, 8 Nms/rad spring. Angular contact compliance is zero, but the
accumulated impulse remains limited to 40 Nm * substep. Requested angular speed
is current controller speed plus shortest-arc orientation correction, capped
at 20 rad/s. Correction reserves discrete braking distance using current
world inertia (hand and welded load), so heavy loads do not enter a full-speed
bang-bang turn near a stationary goal. A stationary goal requests zero spin
once reached. Actual inertia, mass, rigid grip, linear drives, collision solving
and anatomical locomotion limits remain. Releasing the grip returns the wrist
to its free tracking motor; released props retain ordinary simulated momentum.
No prop orientation or angular velocity is assigned outside the contact solver.

Marker: `angular=rate-servo-v2`. Additional `VRGripAngularRate` diagnostics record
requested, pre-solve and current post-solve angular velocity, which can expose
whether a future failed headset test comes from target, solve or integration.

Reduced numerical checks compare spring and rate servo at five dt values,
three inertias and three iteration budgets, plus 36 stationary light/heavy cases
with finite torque, speed cap, monotonic settling and no residual spin. Existing
frame, tracking, loaded-motor and root-reaction checks remain. These are numerical
models/source guards and native compile checks; stability on the headset remains
unconfirmed until the user tests. Logs are read only on the user's command.


## 2026-10-06: sphere inertia and locomotion authority

Sphere mass properties omitted volume in their inertia, despite Create/Add
scaling by mass/volume. For each 4 cm wrist sphere the error was about 3730x.
The corrected unit-density formula affects all spheres, without changing masses.
Held wrist drive now uses the sampled predicted orientation without extra angular
velocity prediction. Bounded torque and finite-inertia braking remain.
Separate capsule/prop pull impulses are removed. Arm reach constrains requested
and coasting living velocity inside the locomotion step, solving both hands
jointly. Release clears the limit; stale limits expire after 0.1 s. Reach uses the
anatomical physical wrist, not the prop anchor. Holding alone no longer offsets
the visible torso through delayed physical segment poses; world contact remains.

Validation: check_vr_native_solver.py compiles current native CryPhysics solver
and extracted production sphere mass properties. Its six-body fixture passes
12 moving-target cases, four static 0.8 rad turns with 0.005 rad overshoot
tolerance, and 20000 two-hand velocity projections. Drive registration is a
fixture approximation; full game broadphase, camera, and OpenXR are not covered.
All five Python regression scripts pass. Release APK installed; all 17 engine
libraries matched packaged stripped libraries and installed APK hash matched.
Headset behavior awaits user verification. No game launch or new log read.


## 2026-10-06: smooth carrier translation

User confirmed rotation is good; reported framewise dragging lag with stick
locomotion. Tracking previously supplied a fixed world target per game frame,
including sampled root movement in the differentiated tracking velocity.
Tracking now includes carrier ID and sampled carrier position. Every native
physics substep translates the target by actual carrier displacement, and adds
current carrier velocity as moving-frame feedforward. Tracking velocity is
differentiated relative to carrier translation so root motion is not counted
twice. Angular sampled-pose servo is unchanged. No held object pose write,
teleport or added pull impulse. Carrier stops at an obstacle through actual
living physics, not requested stick velocity.

Native six-body walking fixture passes 12 mass/timestep combinations with less
than 1 cm steady lag (observed under 1 mm). Existing 16 rotation cases remain
passing. check_vr_carrier_tracking.py covers 16 sample/substep schedules and
10000 relative velocity samples, plus source integration guards. These fixtures
do not validate full game camera/render timing. Release installed, all 17 engine
libraries verified and installed APK hash matched. No new headset log read or
game launch; user verification required.
