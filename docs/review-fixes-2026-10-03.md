# Нанесени поправки — 3 октомври 2026

По находките от `optimization-review-2026-10-03.md`. Онзи документ описва кода **преди тези поправки**; старите му номера на редове са исторически ориентир. Таблицата тук описва реално нанесените промени.

## Логика и стабилност

- Event port: един собственик на close, синхронизиран shutdown и atomic stop/idle. Неподдържан входящ payload затваря само event клиента с еднократно предупреждение; няма цикъл върху непрочетени данни.
- RTSP: restart/replacement не нулира живи клиентски слотове. Maintenance/OTA проверяват idle. След timeout cleanup продължава да пази ownership и повтаря stop, за да почисти ALAC DATA, суспендирала по-късно.
- Crypto: отрицателният read резултат е фатален, независимо от старо errno. Response send има общ 2-секунден deadline, nonblocking send/EINTR обработка; partial encrypted failure затваря връзката.
- SETUP: локално валидиране, след това публикация при idle. Идентичен live повторен SETUP връща съществуващите ports; променена конфигурация се отказва с 455. ANNOUNCE не може да променя работещ producer. AAC държи immutable ключ за живота на задачата.
- Plist: integer marker/width validation, проверки преди uint64→size_t cast, subtraction/division bounds преди offset table, bounded recursion budget и cycle detection.
- Progress: 32-битова модулна RTP разлика и валидиране на входа.
- I2S: handle се публикува след завършена инициализация; грешка освобождава временния channel.
- PTP: сериализиран lifecycle, atomic flags/handle, startup handshake преди task изпълнение и owner-only socket close. Incomplete stop не разрешава init.
- Logs: един broadcaster, idempotent init/register, cleanup при частично init failure, detach преди httpd_stop.
- ALAC/FIFO sockets: shutdown прекъсва четенето; descriptor numbers не се освобождават преди producer idle, за да няма използване на рециклиран descriptor.
- Shared run flags използват acquire/release. Съществуващите FLUSH, generation, seqlock, publication и I2S ownership защити са запазени.

## Производителност и памет

- Main task връща след init: ESP-IDF може да освободи неговите 3584 байта стек.
- AAC decode и ALAC ordered staging споделят 4096-байтов internal scratch. Idle бариерите остават; един allocation/free. WORK scratch остава отделен.
- ALAC startup нулира packet metadata вместо 96 payload масива: спестява около 768 KiB PSRAM записи на старт.
- Enabled neutral stereo EQ пропуска float/filter/quantize пътя и запазва точно trailing-silence counters. Филтри/dither при реална обработка остават.
- PCM contiguous scan проверява 32-битови думи; няма ctz върху нула или shift с 32. Останалите readiness проверки вече са били word-based.
- Log ring копира с най-много две memcpy вместо по байтове; backlog/overflow семантиката остава.
- FIFO `not_empty` semaphore е премахнат; consumer продължава да чака control_wake.
- RTSP header се проверява в receive buffer с guard byte; няма allocation/copy на header при всяко получаване. Crypto scratch се използва повторно на connection и предпочита PSRAM.
- PTP snapshot производните възрасти/деления са изнесени извън spinlock; три неизползвани mirrored legacy fields са премахнати.
- Decoder грешките се обобщават при първата и през 64 грешки; AAC history policy и ALAC reset policy остават.
- Calibration error delay е поне един tick вместо нулев delay.

### Recovery при натоварване

Semantic MISSING/RECEIVED събитията не се губят мълчаливо при full queue: само WORK чака с еднотикови cancellation-aware retries. DATA/CTRL не чакат тази опашка. RESEND обработва до 32 събития преди deadline service; голям missing range се ограничава до съществуващите 512 позиции. NACK/retry/final-loss сроковете са запазени.

Добавени са counters за DATA/RTX drops и MISSING/RECEIVED backpressure. При промяна status задачата печата `ALAC queue pressure: dataDrops=... rtxDrops=... missingWaits=... receivedWaits=...`. При нормален поток няма допълнителен такъв лог. Backpressure може временно да задържи WORK при претоварване; UDP drain остава отделен. Това поведение трябва да се наблюдава при packet loss на устройството.

## Покритие на първоначалните 26 точки

| Точки | Статус |
|---|---|
| 1–8 | Нанесени конкретните поправки/оптимизации. Намаляване на библиотечния ALAC output capacity не е правено. |
| 9 | Header allocation премахнат, crypto scratch reused, framing validation усилена. Response allocation reuse и геометричен receive growth остават кандидати. |
| 10 | Bounded recovery batches/ranges. Snapshot caching, bitmap и промяна на scheduling не са правени без измерване. |
| 11–14 | Нанесени. RTX zero-wait е изрично 0; семантичната опашка има отделна обоснована backpressure политика. |
| 15 | Cold XML/binary scratch, SETPEERS stack redesign и NVS caching/dirty-write остават кандидати; task stacks не са намалявани. |
| 16 | Lifecycle поправен. По-силен viewer backpressure остава кандидат; socket sends запазват HTTPD timeout. |
| 17–26 | Нанесени защитите за bounds/work budget, lifecycle, crypto, SETUP, recovery, shared state и I2S init. Това не е формално доказателство за липса на всички races. |

Крайният PCM буфер остава **128 страници**, RAW капацитетът и Wi-Fi TX конфигурацията са запазени. Съществуващите потребителски промени не са отменяни. VERSION/MANIFEST не са променяни в тази работа.

## Проверка и ограничения

Извършени са source review, преглед на API callers/локални ESP-IDF headers, независим source review и whitespace/diff проверки. Намереният от reviewer delayed DATA reaping дефект е отстранен чрез повторно cleanup. Source whitespace проверката за `main/` е чиста. В PROJECT_CONTRACT.md има съществуващ Markdown hard-break с trailing spaces; той е запазен.

**Не са изпълнявани тестове, компилация или флаш**, по указание на потребителя. Компилируемостта, поведението на устройството и процентът ускорение остават непотвърдени. Общото освобождаване от main stack + scratch е 7680 байта преди разхода за добавени lifecycle mutexes/state; не е измерен net heap gain.
