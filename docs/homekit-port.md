# HomeKit пренасяне — доклад

Работно копие: /Users/kriso/airplay-esp32-homekit, клон homekit-ap2.
Продължена е започнатата работа върху airplay-esp32 при bd30a1f (v4.5.7).
Източник: [homekit](https://github.com/Kristian8606/airplay-esp32/tree/5cfdaad414b3f5bcf3ee4e4be34fa9bc257752f8).
Оригиналните airplay-esp32 и esp32-airplay2-surround не са редактирани; и двете работни дървета са чисти при финалната проверка.

## Реализирано

- Представяне като външна AirPlay колонка (AirPlaySpeaker1,1), с производител, версия, сериен номер и съгласувани mDNS/GET /info данни.
- Пълно сдвояване и първоначално добавяне през шифрована transient сесия; /configure, /pair-add, /pair-remove, /pair-list и /pairings.
- Ed25519 проверка на познатите контролери; отнемане на права при изтриване. Премахване или понижаване на последния администратор изчиства всички сдвоявания.
- Запазени NVS namespace airplay, ключове ed25519_pub, ed25519_sec, hk_pairs и старият формат: 16 записа по 99 байта. Записите във flash се изпълняват с вътрешен стек и вътрешно копие на данните. Неуспешен запис възстановява предишното RAM състояние.
- Динамични HomeKit/relay флагове и gid/gcgl за свързване с групата на източника и показването на обложки/контроли в Home. Повторен SETUP без нова група запазва текущата. Частична грешка при mDNS обновяване допуска последващо възстановяване.
- Управляващите isRemoteControlOnly сесии получават собствен event port, без да придобиват аудиото или да променят PTP, усилвателя и възпроизвеждането.
- Обложки през SET_PARAMETER до 2 MiB, включително шифровани и разделени TCP кадри. Изображението се приема на части и се изхвърля, както в изходния клон; не се съхранява/декодира и няма алокация за целия му размер. Следващата команда запазва правилно подравняване. Рекламира се md=0,1,2 при включени обложки. Не е добавено показване на обложка в уеб страницата.

## Запазено

При първоначалното HomeKit пренасяне нямаше промени в main/audio, main/network, I2S/DMA, декодерите, EQ, часовника, Wi-Fi, OTA или закрепването/приоритетите на съществуващите задачи. Последващият разрешен тест на AAC лимитера променя декодера и публикуването/приключването на buffered аудиото, както е описано по-долу. Аудиоформатите остават тези на текущия проект. DMA остава 8 блока по 480 кадъра — около 80 ms при 48 kHz.

## Решения и ограничения

- Запазен е отвореният AirPlay достъп (acl=0). /configure връща действителното ниво 0, идентификатора и публичния ключ, без да обещава неприлагана парола или ограничения за членове на дома. При домакинство с друго избрано ниво приемането на договореното ниво трябва да се провери с реалния Home app.
- Повторно изпращане на точно същия администраторски идентификатор и публичен ключ получава идемпотентен успех, но не удостоверява връзката като администратор. За последващо управление е необходим подписан Pair Verify. Така публичният ключ сам не предоставя права. Реално повторно добавяне след загубен отговор остава за проверка на устройство.
- Transient M5 не може да записва HomeKit администратор. Пълният M5 проверява повторно за вече появил се собственик между M1 и M5.
- Нечетими/повредени съществуващи сдвоявания връщат грешка при инициализация вместо автоматично изтриване. Сегашният boot код прекратява стартирането при такава грешка; действително повреден NVS изисква отделно възстановяване.
- По-големият DMA не гарантира звук без накъсване при натоварен Wi-Fi. Приемането вече не буферира/копира цялото изображение, но прехвърлените байтове и криптографията продължават да използват радио и CPU.

## Проверки и резултати

Хост командата е python3 tests/homekit/run_tests.py. Всички проверки минават с UndefinedBehaviorSanitizer и реален libsodium за Ed25519/X25519/ChaCha20-Poly1305.

Проверени са M5/M6 обменът на идентичности с валиден подпис, Pair Verify с валиден и невалиден подпис, отнемане на права, NVS commit/read грешки, повредени записи, последен администратор, управляващи връзки при празно/активно аудио, повторни групови настройки, частични mDNS грешки и независим прочит на отговорите с Python plistlib. Тест с 200 KB шифрована обложка, разделени кадри и следваща команда минава без голяма алокация. Специален вложен plist от около 62 KB възпроизведе прекомерно обхождане (9.131 CPU секунди); след поправката завършва под показваната милисекундна резолюция.

FreeRTOS, NVS и аудиохардуерът в хост тестовете са заместители. SRP математиката не се изпълнява там: фиксиран установен SRP ключ служи за проверката на пренесения M5/M6 обмен. Реалният SRP и ESP-IDF код се компилират във фърмуера.

И двете актуални компилации за ESP32-S3 с ESP-IDF 6.0.2 минават:
- HomeKit и тестов AAC лимитер включени: build-homekit/airplay-esp32_v4.5.7.bin, размер 0x1ed7e0 (2 021 344 байта).
- HomeKit и тестов AAC лимитер изключени: build-nohomekit/airplay-esp32_v4.5.7.bin, размер 0x1eaec0.
- HomeKit образът се побира в 3 MiB app дял с 36% свободно място.
- git diff --check е чист. Промените в аудиото за последващия тест на лимитера са описани по-долу.

Няма физически тест, запис на платка, push или commit. За устройство остават: добавяне в Home, поява на обложка, play/pause/volume от Home, рестарт със запазено сдвояване и продължително слушане. Успешната компилация не установява тези резултати.

## Използване

HomeKit вече е включен в активния sdkconfig на копието. Допълващите настройки са в sdkconfig.homekit.
Повторна компилация в ESP-IDF 6.0.2 среда: idf.py -B build-homekit build.
За чиста конфигурация използвай SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.homekit като един аргумент към idf.py. Съществуващ sdkconfig има предимство пред defaults.

Извършен е отделен преглед на целия порт; потвърдените проблеми са коригирани. Няма оставени дребни забележки от този преглед.

Update: removed routine RTSP volume-value logs (incoming SET_PARAMETER and local remote-volume update). Error/warning logs and all volume processing remain. HomeKit ESP32-S3 build passed; firmware not flashed.

## Immediate ownership, deferred buffered playback

The HomeKit build now transfers ownership on the initial normal audio-session SETUP. Pairing and isRemoteControlOnly sessions do not claim audio. The new pairing-derived key and event listener are prepared before the previous owner is stopped; failed preparation leaves the previous playback intact. Successful takeover closes the old owner's session and selects the new sender's clock. Late requests from a closing connection cannot reclaim ownership.

Session selection and AAC stream SETUP do not start playback. RECORD remains an acknowledgment. SETRATEANCHORTIME rate 1 opens the existing playback gate, and the unchanged audio engine still requires valid timing and PCM. Pause and stream-only TEARDOWN preserve the session for a later stream/play command. Non-HomeKit selection semantics remain unchanged.

Regression: reproduced old behavior with a playing phone remaining owner after a second sender's normal session SETUP. New test passes for takeover, waiting before play, repeated SETUP, late old commands, remote-only setup while audio plays, failed listener/key preparation, pause/resume and stream restart. Full host UBSan suite and both ESP32-S3 builds pass. No board was flashed. Physical Apple TV playback and the separately observed event-port allocation failure still require device verification; no claim is made that the unexplained 70ms sender cancellation has been reproduced or eliminated.


## S32 AAC limiter experiment (2026-11-08)

User-selected variant: retain 32-bit PCM and enable the FDK PCM limiter for a
listening test. Stock FDK rejected its limiter with this fork's DecodeFrame32;
libPCMutils now shares the original peak/lookahead/gain algorithm between S16
and S32 output conversions. S32 does not pass through S16. The existing float
DSP and final DMA conversion remain in place. ALAC bypasses this limiter.

Active sdkconfig enables AIRPLAY_AAC_FDK_LIMITER. Its default remains off;
sdkconfig.limiter-test supplies the opt-in setting. Allocation is required at
AAC decoder creation; allocation failure fails creation instead of silently
playing unlimited frames. Additional limiter allocation is approximately
51 KiB in this fork; hardware CPU cost is not measured.

The extra PCM lookahead is 720 frames at 48 kHz (15 ms) or 661 at 44.1 kHz
(about 14.989 ms). PCM RTP tags subtract the actual returned frame delay.
Input sequencing keeps the original packet timestamps. PTP estimation, I2S
clock control, task placement and priorities are unchanged. Measuring timing
after decoding alone cannot discover a waveform's internal lookahead delay.

Muted/new/noncontiguous sequences reset limiter history; muted access units
also reset after decoding so their delayed audio cannot leak into the next AU.
Contiguous pause/resume preserves valid pending samples. Valid final samples
are drained at normal FIN, a forward RTP gap, a same-rate codec switch, or an
approaching DMA deadline without another ready AU. Deadline polling is capped
at 20 ms while a tail is pending. Drain affects only limiter state, without
injecting a synthetic AAC frame. Existing generation/sequence/epoch publication
checks reject audio canceled by stop, FLUSH or replacement streams.

Normal TCP FIN preserves complete queued AUs for this experiment. Incomplete
final packet bytes are discarded after complete AUs drain. A queued replacement
TCP input can supersede the old epoch immediately, even while playback is
paused. The limiter-disabled EOF behavior remains unchanged.

Validation: python3 tests/fdk_limiter/run_tests.py passes with actual FDK code,
bounds instrumentation, locally generated AAC at 48/44.1 kHz, actual PCM FIFO
and loopback TCP with FreeRTOS shims. Tests cover preserved low S32 bits,
attenuation above full scale, S16 compatibility, exact impulse delay, RTP wrap,
muted/seek tails, all final samples, deadline drain followed by contiguous
resume, orderly FIN, partial final packets and replacement input before EOF
acknowledgment. Quiet on/off AAC waveforms match exactly after compensation,
including the final samples. The existing HomeKit UBSan suite also passes.

Current image ELF SHA256:
f0df566c851ddae41c5711b36d3d03e395605e7938a5fda6efa5d464f92d7985.
Both firmware configurations build successfully. No device was flashed for
this change. Acoustic sync, CPU headroom and sustained playback alongside
artwork transfers require device testing. This limiter precedes EQ/loudness;
it does not prevent clipping introduced later by those stages.
