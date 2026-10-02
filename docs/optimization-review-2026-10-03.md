# Преглед за оптимизация — 3 октомври 2026

**Следваща работа:** поправките по този преглед са описани в [Нанесени поправки](/Users/kriso/aairplay/airplay-esp32/docs/review-fixes-2026-10-03.md). Текстът по-долу е анализът преди тях.

Преглед на текущия работен код след връщането на крайния PCM буфер на 128 страници. Цел: по-малко излишна работа, по-нисък разход на вътрешна RAM и по-предвидима обработка, със запазване на работещото аудио.

Това е статичен преглед на собствените модули и избрани използвани ESP-IDF реализации. Не е измерване на CPU, латентност или стекове. Не са правени промени във firmware, тестове, компилация или флаш. Декодерите на Espressif, lwIP, Wi-Fi и криптографските библиотеки не са преглеждани изцяло. Числата по-долу са от размери и алгоритми в кода; не са измерена печалба на устройството.

Най-важните находки за коректност са event-port busy loop, overflow в част от plist object/count проверките, липса на общ work budget при recursive plist search, RTSP restart след неприключил client, смесване на crypto fatal error със старо `errno` и публикуване на format/key върху жив producer. Те имат по-висок приоритет от освобождаване на памет или съкращаване на цикли. Подробностите и условията са в точки 1 и 17–26.

## Приоритети

| Ред | Място | Възможност | Ефект | Риск при промяна |
|---|---|---|---|---|
| 1 | Event port | Премахване на цикъла върху непрочетени данни | Предотвратява непрекъсната CPU работа при входящ payload | Трябва да се уточни обработката на event протокола |
| 2 | `app_main` | Връщане след инициализация | Освобождава 3584 байта стек плюс ресурсите на задачата | Нисък |
| 3 | ALAC startup | Нулиране само на пакетната метаинформация | Избягва около 768 KiB излишни записи в PSRAM при старт | Нисък след проверка на всички потребители на `len` |
| 4 | EQ | Бърз път за действително неутрално стерео | Избягва обработка на всяка семпла при празен EQ | Нисък/среден за преходите на EQ |
| 5 | Logs | Две копирания при wrap вместо цикъл по байтове | По-кратък log hook и задържане на mutex | Нисък |
| 6 | PCM contiguous | Обход по 32-битови думи | По-евтина статистика за непрекъснат PCM | Нужна проверка на граници, пропуски и RTP wrap |
| 7 | AAC FIFO | Премахване на неизползвания `not_empty` | Един semaphore по-малко и по-малко операции при recv | Нисък |
| 8 | PCM scratch | Споделяне между AAC decode и ALAC staging | Потенциално 4096 байта вътрешна RAM | Среден: изисква строг lifecycle |
| 9 | RTSP | Повторно използване на scratch и един header parse | По-малко malloc/free и фрагментация | Среден |
| 10 | ALAC recovery | Един timing snapshot за recovery обход | По-малко cross-core critical sections и сметки | Среден/висок: сроковете трябва да останат верни |

## Конкретни находки

### 1. Event-port задачата може да се върти без блокиране

В [event_port_task](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_handlers.c:273) `select()` следи клиента, а `recv(..., MSG_PEEK)` само поглежда получените байтове. При `n > 0` те остават в сокета. Следващият `select()` веднага връща readable и цикълът повтаря същото без изчакване.

Това е доказуем условен дефект, а не доказателство, че се е случил в изпратения лог. Event task е на Core 0 с приоритет 5, а AAC processor е на същото ядро с приоритет 4. Непрекъснатият цикъл може да лиши AAC processor от CPU.

Корекцията трябва да консумира и обработва очакваните event съобщения, или да прекратява неподдържано съобщение по ясна политика. Само добавяне на sleep оставя непрочетените данни и не решава причината. Задачата има протоколна роля; не е кандидат за сляпо изтриване.

### 2. `app_main` задържа ненужен стек

В [края на app_main](/Users/kriso/aairplay/airplay-esp32/main/main.c:113) остава безкраен цикъл с 10-секунден sleep. Няма текуща работа в него и всички останали подсистеми имат свои задачи. `CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584`.

Локалният ESP-IDF `components/freertos/app_startup.c` извиква `vTaskDelete(NULL)` след връщане от `app_main`. Следователно връщането освобождава този стек чрез нормалното почистване от FreeRTOS. Това е най-простата възможност за допълнителна вътрешна памет без намаляване на работещ стек.

### 3. ALAC startup занулява целите payload буфери

[reset_transport_queues](/Users/kriso/aairplay/airplay-esp32/main/audio/realtime_receiver.c:1054) извиква `memset(slot, 0, sizeof(*slot))` за 64 DATA и 32 RTX слота. Payload частта е 8192 байта на слот: общо 786432 байта, плюс метаинформацията.

DATA приемането записва `len=n`; RTX копира валидния пакет и задава неговата дължина; worker и crypto използват тази дължина. За нормалния lifecycle е достатъчно да се инициализират `len`, `pool_kind`, `retransmitted` и опашките. Не е необходимо зануляване на неизползваната опашка на payload масивите. Това ускорява SETUP/смяната на кодек, а не постоянната обработка.

### 4. EQ без филтри все пак обхожда всяка семпла

[audio_eq_process](/Users/kriso/aairplay/airplay-esp32/main/audio/audio_eq.c:619) има bypass при `enabled=0`, но при `enabled=1`, stereo, 0 dB и нула активни филтри минава през `process_stereo_eq`. То прави int16→float, умножение, `eq_quantize` и `lrintf` за двата канала. При 44,1 kHz това са 88200 такива пътя в секунда, въпреки че сигналът е неутрален.

Подходящ е отделен бърз път за **stereo + нула активни филтри и на двата канала + unity preamp**. Блокът остава битово същият. Трябва да се запази коректното обновяване/нулиране на състоянието при последващо включване на филтри. Mono/left/right не са идентичен пропуск: променят маршрутизацията; mono със средно от две семпли може да изисква закръгляване и dither.

EQ с реални филтри вече използва float и предварително подготвени коефициенти. `tan`/`pow` са при подготовката, не при всяка семпла. По-голяма промяна към ESP-DSP/SIMD или fixed point трябва да се сравни на устройството; не може да се обещае печалба само от кода. Не трябва да се премахват dither и filter history заради скоростта.

### 5. Log ring копира байт по байт

[ring_write/ring_read](/Users/kriso/aairplay/airplay-esp32/main/network/log_stream.c:51) обновяват курсора за всеки байт. Кръговият буфер позволява най-много две `memcpy` операции при преминаване през края и еднократно обновяване на head/tail. Трябва да се запази реалният капацитет `LOG_RING_SIZE-1` и изхвърлянето на най-старите байтове.

[log_vprintf_hook](/Users/kriso/aairplay/airplay-esp32/main/network/log_stream.c:77) форматира към UART и повторно чрез `vsnprintf` за браузъра. Работата се изпълнява в задачата, която пише лога; преместването на `log_ws` в PSRAM не премества този hook извън media/control задачите. Отворен браузър не е условие за hook: той събира backlog и без viewer.

Копиранията могат да се оптимизират без промяна на backlog. Премахването на второто форматиране изисква отделен дизайн, който запазва целия UART текст и правилната употреба на `va_list`; настоящият browser buffer реже съобщенията до 255 байта. Простото пропускане без viewer променя наличния backlog.

### 6. PCM contiguous статистиката може да използва думи

[validity_has_range](/Users/kriso/aairplay/airplay-esp32/main/audio/pcm_rtp_ring.c:114) **вече проверява цели думи с крайни маски** и не се нуждае от тази оптимизация. [pcm_rtp_ring_contiguous_frames](/Users/kriso/aairplay/airplay-esp32/main/audio/pcm_rtp_ring.c:494) копира битовата маска под seqlock и после проверява по един кадър.

Може да се проверяват цели 32-битови думи, с маски за първата/последната и намиране на първия нулев бит. Една пълна страница става до 32 проверки вместо 1024 проверки на кадри. Това не означава измерено 32-кратно ускорение на функцията: копирането, seqlock и другите разходи остават.

Основният PCM→I2S lookup е директен, без сортиране. Поколенията инвалидизират в O(1). Те са добри решения. Не трябва да се премахват seqlock, RTP page tags, generation rechecks или writer mutex заради по-малко инструкции.

### 7. AAC FIFO има неизползван semaphore

`not_empty` в [ap2_buffered_fifo.c](/Users/kriso/aairplay/airplay-esp32/main/audio/ap2_buffered_fifo.c:41) се създава, сигнализира и изтрива, но никъде няма `xSemaphoreTake` върху него. Consumer чака `control_wake`. Премахването му спестява един semaphore и излишното Give след recv/notify. Печалбата е малка, но условието е ясно.

Reader вече приема директно в PSRAM FIFO, без допълнителен временен receive buffer. Peek копира само header; discard премества курсора без payload copy. Те трябва да се запазят. Задържането на бъдещи AAC кадри компресирани е важно за AutoMix, а не излишна логика.

`accept` на nonblocking listener се проверява през 20 ms и full FIFO през 100 ms. Те могат да станат event/socket-driven, но имат нисък приоритет: reader съществува само при активен buffered stream. 10-ms pacing след recv е изрично приетото поведение в проекта; премахването му може да създаде CPU/network bursts.

### 8. Два PCM scratch буфера могат да споделят памет

[audio_receiver_init](/Users/kriso/aairplay/airplay-esp32/main/audio/audio_receiver.c:3172) заделя `s.decode_pcm` и `s.realtime_stage_pcm`, всеки 1024 stereo int16 frames = 4096 байта вътрешна RAM. Първият се използва от AAC processor, вторият от ALAC ordered staging.

Кодековите start проверки вече изискват предишните producers да са спрели. При запазване и доказване на тази граница те могат да бъдат един буфер. Потенциалното спестяване е 4096 байта без допълнително копиране и без PSRAM достъп по този път. Release/free трябва да има един собственик; не трябва да се освобождават два alias указателя.

ALAC worker има отделен PCM буфер `s_rt.pcm`: той се използва едновременно със staging, така че **не може** да се обедини с неговия scratch. Намаляване на неговите 1024 frames до договорените 352 е отделен кандидат (2688 байта), но първо трябва да се проверят минималният output buffer и изискванията на библиотечния декодер. Тази печалба не е потвърдена.

### 9. RTSP прави много малки allocations и повтаря parsing

[process_rtsp_buffer](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_server.c:105) заделя и копира header дори когато още чака останалото body. `rtsp_request_parse` после отново търси и парсва headers. Криптографският [read/write](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_crypto.c:30) прави malloc/free за всеки encrypted block до 1024+16 байта. [Response helper](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_message.c:159) заделя и копира дори обикновен `200 OK` без body.

Подходящи са bounded header parsing, запазен parse резултат за текущия request и повторно използван connection scratch за crypto/response. Приоритет 17 трябва да прави по-малко allocator работа. Scratch не трябва да бъде голям нов масив върху `rtsp_client` стека или да се споделя с незащитен втори потребител.

Receive buffer скача от 4 KiB направо на 256 KiB. Постепенно нарастване според валидиран Content-Length може да намали PSRAM разхода при средни заявки. Честото realloc/copy също има цена; нужно е разумно геометрично нарастване и запазване на guard byte и максимума. Голям request и handshake могат да съществуват по време на control, затова не трябва да се заменят с прекалено малък твърд лимит.

### 10. ALAC recovery може да намали сканирането и snapshot разхода

[resend_task](/Users/kriso/aairplay/airplay-esp32/main/audio/realtime_receiver.c:889) вече спи безкрайно при нула липси — това е правилно. При активни липси изпълнява 10-ms recovery цикъл с `resend_giveup_expired`, `missing_find_min` и `resend_scan_due`, които обхождат до 512 позиции. Deadline callback за всяка активна позиция взема audio state snapshot и пресмята RTP→time.

Кандидати: един валиден timing snapshot за обхода, отделяне на сметките от повторните critical sections, bitmap на активните позиции, обединен expire/due обход или планиране по най-близък срок. Bitmap добавя състояние и RAM; не е автоматично по-добър от скан на малък масив.

Не трябва да се сменят first request/retry/last request/final loss margin заради скорост. Нов timing snapshot е нужен на следващия обход и след FLUSH/GM промяна. Не се препоръчва сливане на DATA, CTRL, WORK и RESEND: те разделят UDP drain от decode и recovery.

### 11. Част от малките timeout стойности реално са нула ticks

`CONFIG_FREERTOS_HZ=100` означава tick 10 ms. `pdMS_TO_TICKS(2)` в [ALAC queue send](/Users/kriso/aairplay/airplay-esp32/main/audio/realtime_receiver.c:396) и RTX pool receive е нула: операциите са nonblocking, въпреки написаните 2 ms. Това не трябва да се сменя механично с един tick, защото тогава worker/control може да чака 10 ms.

В [ADC reader error path](/Users/kriso/aairplay/airplay-esp32/main/audio/latency_cal.c:79) `vTaskDelay(pdMS_TO_TICKS(5))` също става нула, въпреки коментара за избягване на въртене при грешка. Там реално изчакване поне един tick е логично. Калибрацията е включена в текущия sdkconfig, но този път работи при измерване, не при нормално AirPlay.

### 12. Decoder error logs могат да се превърнат в burst

[AAC decode](/Users/kriso/aairplay/airplay-esp32/main/audio/aac_decoder.c:133) и [ALAC decode](/Users/kriso/aairplay/airplay-esp32/main/audio/alac_decoder.c:127) логват всяка decoder грешка. Receiver wrappers имат ограничени/редки логове, но вътрешните decoder съобщения не са ограничени. При серия лоши кадри UART и второто форматиране за WebSocket могат да станат значителна работа в media task.

Подходящ е един собственик на брояч и rate limit, с първа грешка и периодично обобщение. Да се запазят реалните error кодове и различието между decrypt/decode/sink failure. Не трябва да се добавя AAC decoder reset при обикновена грешка: запазването на историята е изискване на проекта.

### 13. PTP critical sections имат излишна производна работа

[ptp_clock_get_snapshot](/Users/kriso/aairplay/airplay-esp32/main/network/ptp_clock.c:1294) и realtime snapshot пресмятат възрасти с 64-битови деления под cross-core critical section. Може под lock да се копират сурови timestamps/identity/offset, а производните възрасти да се смятат след него. Проверка/обновяване на stale lock трябва да запази един собственик и съгласуван snapshot.

Legacy полетата `previous_offset`, `previous_offset_time_ms`, `mastership_start_ms` се записват/нулират, но нямат потребители в текущия файл; реалните estimator полета са в legacy engine и `rt_*`. Премахването на тези дублиращи стойности е малко почистване, не голяма CPU/RAM печалба. Не трябва да се сливат автоматично двата PTP режима: qualification и handover семантиките им се различават.

### 14. Metadata progress има доказуем RTP-wrap дефект

[parse_progress](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_handlers.c:1702) чете start/current/end като uint64 и изважда без 32-битов RTP wrap. При end след wrap и start преди wrap се получава огромна продължителност. Това съответства на `39273145:34` в изпратения ALAC лог.

Нужна е разлика по модул 2^32 за RTP границите, плюс валидиране на входа. Това поправя metadata/логовете; не е обяснение за липсвалия PCM и не ускорява самото възпроизвеждане.

### 15. Cold control буфери и stack headroom

`handle_get` има static XML response 4096 байта и binary response 1024 байта във вътрешния BSS. Един споделен response scratch в PSRAM е кандидат при запазване на сериализираното RTSP ownership. Това е преместваема памет, а не ненужен протоколен код.

`handle_setpeers` държи `parsed[16]` с 4 адреса по 64 символа на peer, плюс `tracked[16]`, върху стека. Само parsed масивът е приблизително 4,4 KiB при ESP32 layout. Това е причина да не се намалява `rtsp_client` stack на сляпо. Стрийминг parser към callback или контролен PSRAM scratch може да намали стека, но е отделна промяна.

`settings_get_wifi_network(i)` отново зарежда целия NVS blob за всеки запис в boot selection loop. Еднократно зареждане на списъка е по-евтино, но засяга старта, не audio hot path. Volume вече се пази в RAM и се записва при disconnect; допълнително сравнение със записаната стойност може да избегне ненужен NVS write.

### 16. WebSocket lifecycle и backpressure

`broadcast_task` работи постоянно; `log_stream_register` няма task handle/guard, а `web_server_stop` не отделя `s_server` от log module. Ако публичните stop/start API се използват, може да остане задача със стар server handle и да се създаде втори broadcaster. Текущият нормален boot стартира web server веднъж; това е lifecycle риск при бъдещ/изричен restart, не доказано текущо дублиране.

Локалната ESP-IDF реализация на `httpd_ws_send_frame_async` изпраща през `sess->send_fn` в извикващата задача. Името не гарантира моментално връщане: бавен viewer може да задържи broadcaster до socket timeout. Това не блокира hook mutex, който е освободен преди send. При оптимизация трябва да има ограничено изпращане/backpressure и безопасен server/session lifecycle; опашка към HTTP task не бива да сочи към stack buffer след връщане.

## Логика, състезания и поведение при натоварване

Следващите находки допълват оптимизациите. „Доказуем“ означава, че условието и последствието се виждат в кода; не означава, че дефектът е наблюдаван на устройството. „Условен риск“ означава, че зависи от конкретно преплитане на задачи или необичаен вход. Приоритетът тук е коректността преди спестяването на инструкции.

### 17. Висок приоритет: plist count проверките не са навсякъде overflow-safe

В [bplist_find_data_in_dict](/Users/kriso/aairplay/airplay-esp32/main/plist/bplist_parser.c:561), recursive търсенето и няколко stream/dictionary функции има проверки от вида `pos + dict_size * 2 * ref_size > plist_len`. На ESP32 `size_t` е 32-битов. Голям count може да препълни умножението/събирането, така че проверката да мине, а последващият цикъл да чете извън input buffer. Например `dict_size=0x80000000` и `ref_size=1` зануляват произведението `dict_size*2`. Малък plist може да носи такъв extended count; валидният trailer не доказва валидността на object payload.

[bplist_parse_count](/Users/kriso/aairplay/airplay-esp32/main/plist/bplist_parser.c:325) също не проверява, че extended-count marker е integer, че ширината е допустима и че uint64 count се побира в `size_t`. Read-string/data helpers имат собствени събирания и Unicode умножения, които трябва да се уеднаквят.

Нужен е общ validated object context с граница преди offset table и проверки чрез изваждане/деление: първо `pos <= end`, после `count <= (end-pos)/(2*ref_size)`. Проверка преди cast, допустими integer ширини и отделни bounded object readers. По-новият peer parser вече използва по-добрия стил на проверки; той може да служи като образец. Това е доказуем дефект при malformed вход, с риск от crash/дълъг CPU цикъл, а не предложение да се премахне parsing за скорост.

### 18. Висок приоритет: stop timeout не е бариера за RTSP restart

[rtsp_server_stop](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_server.c:513) предупреждава, ако client не е приключил, но връща `void`. [server_task](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_server.c:362) при следващ старт безусловно нулира двата client slots. `rtsp_server_start` проверява server task, но не всички client tasks.

Ако server task вече е приключила, а client е задържан в send/cleanup, stop може да върне след timeout. Новият server заличава handle/conn/socket на още живия client; той продължава през същия slot и може да засегне нов сокет или новото глобално audio ownership. Това е условен lifecycle дефект. Web Wi-Fi scan/калибрацията имат допълнителни media проверки, но те не доказват, че всички RTSP клиенти са приключили; OTA failure също използва stop/start.

Stop трябва да връща резултат и restart да отказва, докато **всички** клиенти и server са потвърдили края си. Slot нулиране е допустимо едва след тази бариера. Dynamic TCB allocation и настоящото изчакване при обикновена client replacement са добри защити и трябва да останат.

### 19. Висок приоритет: encrypted read използва старо errno за фатална грешка

[rtsp_crypto_read_block](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_crypto.c:30) вътрешно продължава при EAGAIN/EINTR, за да запази частично прочетен frame. При невалидна дължина, allocation failure или authentication failure връща `-1`, без да задава `errno`. Успешно `recv` не изчиства старо `errno`.

В [client_task encrypted branch](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_server.c:234) `block_len<=0` води до повторен read при `errno==EAGAIN/EWOULDBLOCK`. Ако преди фаталната грешка е имало timeout, старото `errno` може да превърне невалиден/auth-failed frame в „изчакай още“. При rejected length следващите payload байтове стават нов length prefix; при auth failure nonce не напредва. И двата сценария могат да оставят control stream без възстановяване.

Нужен е отделен status за idle/cancel/fatal. В настоящия helper idle timeout не излиза навън, следователно неговият `-1` следва да приключва connection. Това е корекция на error contract между две функции.

### 20. RTSP send няма собствен ограничен срок

Client socket има `SO_RCVTIMEO`, но не задава `SO_SNDTIMEO`. [send_all](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_crypto.c:16) и аналогът в `rtsp_message.c` използват blocking send до изпращане на всичко. При клиент, който не чете, control task може да остане в send много по-дълго от очаквания control budget. Това е блокиране, не CPU spin; shutdown от stop обичайно го прекъсва.

Нужни са ограничен send deadline и ясно прекратяване при частично изпратен encrypted frame. Не трябва да се ретраира цял frame след частично send, защото това нарушава framing/nonce. EINTR може да се обработи отделно. Това намалява зависимостта на lifecycle от далечния TCP peer.

### 21. Повторен SETUP може да промени format/key на жив producer

[handle_setup](/Users/kriso/aairplay/airplay-esp32/main/rtsp/rtsp_handlers.c:1193) публикува stream type/format и после encryption, преди да установи дали предишният producer е quiesced. [audio_receiver_set_encryption](/Users/kriso/aairplay/airplay-esp32/main/audio/audio_receiver.c:3410) копира цялата структура без lock, а AAC processor директно я чете при decrypt. Ако SETUP се повтори върху активен AAC поток, или stop е изтекъл по timeout, има възможност за смесен key/type snapshot. ALAC пази собствен config snapshot, което избягва този конкретен read race, но може да остане с конфигурация, различна от глобално публикуваната.

Това е условен риск при неочаквана последователност; нормален TEARDOWN→SETUP с приключили producers го избягва. SETUP трябва първо да парсне и валидира локална временна конфигурация. Идентичен повторен SETUP може да е idempotent; промяна на жив stream изисква потвърден stop или отказ. Key се фиксира като immutable session snapshot за AAC producer, вместо lock/копие на всяко decrypt. Просто заключване само на setter не пази незаключен reader.

### 22. Recovery queue overflow мълчаливо губи смислови събития

[queue_resend_event](/Users/kriso/aairplay/airplay-esp32/main/audio/realtime_receiver.c:388) връща false при full queue. Worker игнорира резултата на MISSING/RECEIVED събитията. При загубено MISSING събитие recovery никога не научава за липсата; при загубено RECEIVED остава ненужна липса до deadline и може да изпраща излишни RTX заявки. Капацитетът 512 ограничава вероятността, но не доказва, че overflow е невъзможен при flood, burst или задържана RESEND задача.

Първа стъпка: евтини counters за dropped DATA/RTX/MISSING/RECEIVED и queue high-water, публикувани рядко. След това може bounded coalescing/pending-state reconciliation със строг single-owner протокол. Не трябва да се заменя nonblocking send с дълго блокиране в UDP/control path или да се логва всяка загуба. В [resend_task](/Users/kriso/aairplay/airplay-esp32/main/audio/realtime_receiver.c:904) drain-until-empty също няма event/time budget: при постоянно попълване може да отлага deadline scan. Bounded batch плюс обслужване на сроковете решава този условен overload риск.

### 23. Atomic lifecycle и socket ownership са непоследователни

ALAC `running/live_tasks`, FIFO epoch и I2S SPSC indexes имат explicit acquire/release. В RTSP/event/PTP част от shared stop flags/task handles/sockets се четат и пишат от различни задачи като обикновени или `volatile` полета. `volatile` не е завършена синхронизация между ядрата и не пази многополево състояние.

Event stop например проверява `event_client_socket`, а owner може между проверката и shutdown да го затвори и друг модул да получи същия descriptor. FIFO client detach-before-close под mutex вече показва добра ownership защита; event/PTP/server пътищата нямат навсякъде същия договор. Това е условно състезание, не твърдение за наблюдаван crash.

Препоръка: един owner на close, синхронизиран stop request/ack, пазен socket snapshot и защита срещу reuse. Приложението не трябва да държи spinlock през socket API; mutex или отделна control команда е подходяща според пътя. Public volume helpers в `rtsp_server.c` също вземат `conn` без lifetime защита; в текущото дърво няма външни call sites, затова рискът е латентен, а не активна причина за дефект.

### 24. PTP повторна инициализация след неуспешен stop

[ptp_clock_stop](/Users/kriso/aairplay/airplay-esp32/main/network/ptp_clock.c:1078) поставя running=false и след timeout само предупреждава. `ptp_clock_init` проверява running, но не task_handle, и след това `memset` нулира целия ptp state. Повторна инициализация преди истински exit би позволила на стария task да работи с новите sockets/state. В текущия код init се вика при boot; stop се използва от v1 transport path, така че това е риск на публичния API при бъдещ restart, а не доказан текущ boot проблем.

Init трябва да изисква пълно idle състояние. Stop result/ack трябва да е единен с RTSP lifecycle. `task_free_spiram` в текущия helper е no-op, така че тук няма доказуем free на още жив статичен стек.

### 25. Depth limit не ограничава общия труд на recursive plist search

[bplist_find_data_recursive](/Users/kriso/aairplay/airplay-esp32/main/plist/bplist_parser.c:612) ограничава дълбочината до 10, но не общия брой посетени objects. References могат многократно да сочат същия array/dictionary или да образуват цикъл. Depth limit спира безкрайната рекурсия, но разклонено повторно посещение може да произведе огромен брой обходи в малък вход. Това работи в RTSP task с приоритет 17 и може да лиши Core0 AAC/network application tasks от време.

Нужен е общ visit/work budget на request и откриване на references в текущия recursion path; по желание memoization на безрезултатни поддървета. Ограничението трябва да е по валидирания брой objects/refs и очакваната schema. Това е конкретна защита срещу натоварване и добра причина за един parse context, а не за по-голям task priority.

### 26. I2S init оставя частично създаден channel при грешка

[audio_playout_init_current_core](/Users/kriso/aairplay/airplay-esp32/main/audio/audio_playout.c:159) задава `s_tx` при успешно `i2s_new_channel`, но при последващ init/register failure връща без channel cleanup. Следващият `audio_playout_init` вижда `s_tx!=NULL` и връща ESP_OK, въпреки че предишната инициализация не е завършила. Това е доказуем error-path дефект; при нормалния успешен boot не се проявява.

Нужни са локален временен handle, cleanup при всяка грешка и публикуване на `s_tx`/ready чак след завършен init. Аналогично audio_receiver init е поетапен и може да остави полезни allocations за retry; критерият му за initialized обаче трябва да включва готовите workers и I2S, а не само engine flag/stores. Не трябва да се освобождават обекти, които вече са видими на стартирана задача.

### Проверени защитни механизми, които трябва да останат

- PCM page writer има bounded CAS, mutex за writers/invalidation и seqlock за readers; няма безкрайно въртене на high-priority reader върху preempted writer.
- AAC publication и FLUSH придобиват control→publish locks в еднакъв ред; state snapshots и revision/epoch проверки предотвратяват стар decoded PCM да се върне след FLUSH. Не е намерен обратен ред в проверените call paths.
- I2S tags/completions са SPSC с release/acquire и вътрешна ISR памет. Само playout owner прави flush/write/tune; грешка в tag/write води до re-prime, вместо фиктивно придвижване на cursor.
- ALAC decoder/seen state има WORK owner, missing state — RESEND owner, D7 commit — CTRL owner. Това разделяне предотвратява няколко класа races.
- Codec workspace се споделя между AAC и ALAC само след idle проверки. Wi-Fi release отказва free при активни producers/workers. Тези бариери не са излишен overhead.
- EQ config/process използват mutex; coefficients се подготвят извън дългия apply lock и live state се пази. Дължината на process lock остава кандидат за измерване при максимален брой филтри; премахването на lock без immutable config publication е неправилно.

## Преглед по подсистеми

| Подсистема / функции | Извод за текущата архитектура |
|---|---|
| AAC FIFO: reader, peek, visit, consume, epoch, stop/destroy | Директно приемане, един consumer, token validation и backpressure са полезни. Основната дребна излишна работа е `not_empty`; stop polling може да стане acknowledgement. |
| AAC processor: classify, SSRC selection, lead gate, crypto/decode, mute, PCM publication | Запазването на бъдещи компресирани кадри и decoder history е необходимо. Не се препоръчват втори cursor, fast skip или декодиране на preload. |
| FLUSH: immediate/deferred/no-boundary, control revision и generation | Lock ordering и повторната проверка след decode предотвратяват връщане на стар PCM. Scan е bounded и по control събитие; не е първа CPU цел. Експерименталният zero-seq policy остава отделен функционален въпрос от оптимизацията. |
| ALAC: DATA/CTRL/WORK/RESEND, seen/missing tracking, D7 anchor | Разделянето на приемане от decode е разумно. Startup memset и recovery обхожданията са конкретните цели. |
| ALAC ordered staging и raw/final PCM rings | RAW пази recovery/реда на EQ. Стековете и таблиците не са излишни само защото се чака. Крайният буфер остава 128 страници; вече е наблюдаван проблем при 64. |
| I2S: tags/completions, ISR callbacks, preload/flush/write/tune | Малки SPSC опашки, ISR timestamps и един owner на I2S са добри. DMA/ISR данни остават вътрешни. Не се премахва повторното валидиране на startup. |
| Timing/servo: snapshot, wanted RTP, deadlines, PID, center, tune | Най-горещата повторна работа е snapshot/time преобразуването. PID е веднъж в секунда, tune е ограничен; double→float там има по-нисък приоритет от EQ/hot-path работа. |
| EQ/volume/dither | Предварителни коефициенти и unity/mute volume fast paths вече са налични. Неутралният enabled EQ е пропуснат бърз път. Volume ramp с 64-битово деление е само при промяна на volume. |
| RTSP framing, dispatch, SETUP/TEARDOWN, encrypted blocks и event socket | Scratch reuse, bounded single parsing и event-port defect са приоритетите. Session replacement сериализацията пази глобалното audio ownership. |
| Plist/XML/base64/TLV | Контролни, сравнително редки пътища. Един validated plist context може да намали повторните trailer/dictionary scans. При refactor трябва да се уеднаквят overflow-safe length/count проверки; проверките не се премахват за скорост. |
| HAP/SRP/RSA/FairPlay и ключове | Скъпа работа при handshake, не на всяка PCM семпла. Да се ограничи allocator/stack pressure, а не да се отслабва crypto или да се премахват handshake пътища само по коментар „legacy“. Това не е пълен security audit. |
| Wi-Fi reconnect/scan, DNS, mDNS | Backoff, запазен BSSID и освобождаване на audio workspace при scan са полезни. DNS спира след свързване. Boot NVS list reload и bounded scan result allocation са второстепенни цели. |
| Web UI/JSON/SPIFFS/OTA | Ниски приоритети и потоков OTA вече ограничават паметта. Настройки/OTA правят flash операции; стековете на тези задачи не се местят сляпо в PSRAM. JSON allocation failure handling може да се уеднакви. |
| LED VU и amplifier | LED копира ограничено до 30 Hz, RMS/цветове са в low-priority task. Amplifier чака notification/timeout. Не се вижда основание за сливане с media задачи. |
| Audio diagnostics | `audio_diag.c` не се компилира при изключена диагностика; изтриването му не освобождава runtime RAM в текущия build. |
| Helpers/build configuration | `spiram_task.h` всъщност създава вътрешни стекове; името трябва да се изясни, а не глобално да се променя поведение. Performance compiler optimization и I2S IRAM-safe режим вече са включени. |

## Предложен ред за работа

1. Поправки за коректност: event-port цикъл, plist overflow/work budget, RTSP stop/restart и crypto error contract; след тях transactional SETUP, bounded send и progress wrap. Всяка е отделна reviewable промяна.
2. Освобождаване на main task, премахване на неизползвания FIFO semaphore и payload memset при ALAC старт.
3. EQ neutral fast path и memcpy-based log ring, без смяна на филтри/dither/backlog.
4. Оптимизация на PCM validity алгоритмите при запазване на seqlock и всички граници.
5. Споделен AAC/staging scratch и RTSP scratch reuse след преглед на ownership.
6. Recovery/timing оптимизации само след измерване под реални packet loss и GM/FLUSH преходи.

Main stack и споделеният PCM scratch дават потенциално общо 7680 байта (7,5 KiB) допълнителна вътрешна RAM, без да се намаляват PCM ring капацитетите. Споделеният scratch още не е реализиран или проверен. Печалби от други премествания/намалявания не са включени в тази сума.

За оценка на изпълнени оптимизации са нужни времена за block decode/EQ/staging, време на recovery обход при липси, CPU idle и minimum/largest internal heap, плюс underrun/late RTX поведение. Няма основание да се обещава процент ускорение преди такива измервания. По текущото указание тук не са изпълнявани тестове.
