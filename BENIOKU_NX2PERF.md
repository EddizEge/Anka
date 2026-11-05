# NX2Perf

NX2Perf, **PokeDinner-Eglantine 2.0.0**'ın (Franseven'in Eden emülatörü çatalı) resmî olmayan bir
performans çatalıdır; amacı **Donkey Kong Bananza**'yı masaüstü PC'de daha akıcı oynatmaktır.
Tek bir program içerir: `eden-cli-nx2perf.exe` (komut satırı / SDL ön yüzü). Bu program mevcut bir
PokeDinner-Eglantine 2.0.0 kurulumunun yanına konur; başlatıcılar onu ölçtüğümüz ayarlarla açar.

Eden projesiyle ya da Franseven'le bir bağı yoktur. Sorunlarını onlara bildirmeyin; bu çatalın
sorun izleyicisini kullanın (`<KAYNAK_URL>`).

- Taban: PokeDinner-a-Eglantine-Fork 2.0.0, commit `70ecb39ef0`
  (https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork)
- Bu sürüm: paketteki `VERSION_NX2PERF.txt` (etiket, commit, derleme tarihi).
- Kaynak: `<KAYNAK_URL>` (programla aynı commit; GPLv3, bkz. "Lisans").
- Değişiklikler: `CHANGELOG_NX2PERF.md` (İngilizce). Bütün anahtarlar: `nx2_switches_NX2PERF.txt`
  (kaynakta `dist/nx2_switches.txt`).

**Tek makinede, tek oyunla denendi.** Aşağıdaki bütün sayılar Ryzen 7 5700X (8 çekirdek), 64 GB
RAM, NVIDIA GeForce RTX 4060 Ti ve 1440p 170 Hz ekranlı bir makinede Donkey Kong Bananza
oynanırken alındı. Başka oyunlar, başka işlemciler, AMD ya da Intel ekran kartları denenmedi.

## Gerekenler

- Windows 10 ya da 11, 64 bit.
- Çalışan bir **PokeDinner-Eglantine 2.0.0** kurulumu: kendi anahtarlarınız, firmware'iniz ve
  oyun dökümünüz, 2.0.0 için kurulduğu gibi. NX2Perf bunların hiçbirini sağlamaz, sağlamayacak.
- **Microsoft Visual C++ 2015-2022 x64** çalışma zamanı (2.0.0 da ister). Program, 2.0.0'ın
  `eden-cli.exe`'siyle birebir aynı DLL'leri kullanır; başka bir şey kurmak gerekmez.
- Güncel **Vulkan** sürücülü bir ekran kartı (2.0.0'daki gibi). Yalnız NVIDIA RTX 4060 Ti denendi.
- x86-64 işlemci. Derleme AVX2 istemez (AVX2 kodu yalnız Opus ses kodlayıcısında var ve işlemciyi
  çalışırken denetliyor), ama yalnız Ryzen 7 5700X denendi. Değişikliklerin çoğu GPU iş
  parçacığını hızlı bir masaüstü işlemcide hızlandırır; yavaş işlemcide kazanç daha az olur.
- Çok çekirdekli CPU öykünmesi açık (varsayılan). Kare zamanlama anahtarları bunu ister.
- İsteğe bağlı: ekranda FPS / hız göstergesi için RivaTuner Statistics Server (RTSS).

## Kurulum

1. PokeDinner-Eglantine 2.0.0 klasörünü açın (`eglantine.exe` ve `eden-cli.exe`'nin olduğu).
2. NX2Perf zip'inin içindekilerin hepsini oraya kopyalayın. Hiçbir dosyanın üstüne yazılmaz:
   programın kendi adı var (`eden-cli-nx2perf.exe`), 2.0.0 eskisi gibi çalışmaya devam eder.
3. NX2Perf, 2.0.0'ın `eden-cli.exe`'siyle aynı kullanıcı klasörünü kullanır (taşınabilir
   kurulumda programın yanındaki `user\`, değilse `%APPDATA%\eden`): aynı anahtarlar, kayıtlar,
   ayarlar. eden-cli ayarlarını oradaki `config\sdl2-config.ini`'de tutar (Qt programı
   `eglantine.exe` `qt-config.ini`'yi kullanır; ikisi ayrıdır). Bu dosya yoksa programı bir kez
   açıp kapatın.

Kaldırmak için zip'in eklediği dosyaları silin. Yeni programın kurduğu gölgelendirici önbellekleri
kullanıcı klasöründe kalır; durmalarında sakınca yok.

## Başlatıcılar

Oyun dosyanızı bir başlatıcının üstüne sürükleyin ya da `.bat`'ı metin düzenleyicide açıp yolu
`GAME=` satırına yazın. Her başlatıcının başında değiştirilmek için konmuş satırlar var:

| Satır | Anlamı |
|---|---|
| `GAME=` | açılacak oyun (sürüklenen dosya önceliklidir) |
| `CONFIG=` | bu başlatıcı için başka bir `sdl2-config` .ini; boş = eden-cli'nin kendi ayarı |
| `FULLSCREEN=-f` | tam ekran; boş = pencere |
| `DK_DYN=1` | Bananza dinamik kare hızı yaması (aşağıda); `0` = kapalı |
| `OSD=1` | RTSS ile FPS / hız göstergesi; `0` = kapalı |

(`DK_DYN=` ve `OSD=` yalnız önerilen ve reaktif başlatıcılarda var.) Oyunun yolunda `&`, `^`, `,`,
`;` ya da `=` varsa dosyayı sürüklemeyin, yolu `GAME=` satırına yazın. `CONFIG=` satırındaki dosya
yoksa başlatıcı bir iletiyle durur (eden-cli sessizce varsayılan ayarlarla yeni bir dosya
yazardı); emülatör hatayla kapanırsa pencere açık kalır. Başlatıcıları yayın zip'inden alın:
Windows satır sonları (CRLF) gerekir.

- **`start-recommended.bat`** (önerilen): bizim oynadığımız ayarlar (Donkey Kong Bananza).
  GPU iş parçacığının arka tarafı aşamalı (`BACK_STAGES=1`, `BACK_CUTS=a`), `ACQUIRE_PEEK=1`,
  `JIT_SAME=1`, `FRONT_MACRO_LEAN=1`; Bananza'nın kendi 60/30/20 FPS seçimi doğru hızda izlenir
  (`PRESENT_INTERVAL_WORD=12:16:4`, `MODE_PACE=2`, `PACE_SPIN_US=1500`); dinamik kare hızı yaması
  (`DK_DYN=1`; yama uygulanınca yukarıdaki iki hız satırı devre dışı kalır: `DK_DYN=0` iken ya
  da yama uygulanmazsa etkilidirler); zamanlı FSR3 ara kareleri (`FG_TIMED=1`, yalnız kare üretme açıkken etkili); RTSS
  göstergesi (`OSD=1`). Bananza'ya özel satırlar işaretli; başka oyunlar için güvenli başlatıcıyı
  kullanın ya da o satırları silin.
- **`start-safe.bat`** (güvenli): bu çatalın eklediği bütün anahtarlar kapalı (varsayılanı açık
  kısayollar `0`, isteğe bağlı olanlar boş). Programın 2.0.0'a en yakın hâli; bir sorunun yeni
  anahtarlardan gelip gelmediğini anlamak için. Anahtarı olmayan değişiklikler (değişiklik
  günlüğündeki doğruluk düzeltmeleri ve upstream'den alınanlar) yine içeridedir.
- **`start-reactive-experimental.bat`** (reaktif, DENEYSEL; önerilenin yanında ikinci seçenek):
  reaktif boşaltma açıkken önerilen ayarlar, artı `RX_RT_FIT=3` (Bananza'nın açılışındaki çizim
  hedefi için çizim başına bir kopya yerine tek görüntü). Bizim makinede açılışı geniş alanlara
  değişir: reaktif kapalı aynı derlemeye göre oyunun açılışında FPS −%7, geniş alanda +%15. Henüz
  denenmedi: kare üretmeyle birlikte çalışması ve `RX_RT_FIT=3`'ün görüntüsü. Reaktif boşaltma bir
  anahtar değil, bir ayardır: aşağıda "Reaktif boşaltma".

eden-cli anahtarları yalnız ortamından (başlatıcılardan) okur. `nx2_switches_NX2PERF.txt` her
anahtarı varsayılanı ve ne yaptığıyla listeler. (2.0.0'ın kendi `nx2_switches.txt`'sini yalnız Qt
programı okur; paket onu değiştirmez.)

## Başlıca anahtarlar

Hepsi `EDEN_NX2_<AD>` adlı ortam değişkenleridir. Tam liste (yaklaşık 230 yeni ad, çoğu tanı ya da ince ayar)
`nx2_switches_NX2PERF.txt`'de.

| Anahtar | Varsayılan | Ne yapar |
|---|---|---|
| `BACK_STAGES=1`, `BACK_CUTS=a` | kapalı | bölünmüş GPU iş parçacığının arka tarafı aşamalar hâlinde çalışır |
| `ACQUIRE_PEEK=1` | kapalı | zaten karşılanmış bir GPU semafor beklemesi arka tarafı boşaltmadan geçer |
| `JIT_SAME=1`, `FRONT_MACRO_LEAN=1` | kapalı | ön tarafın makro JIT'i hiçbir şeyi değiştirmeyen yazımları atar |
| `PRESENT_INTERVAL_WORD=12:16:4` | kapalı | sunumlar oyunun kendi kare aralığını izler (Bananza) |
| `MODE_PACE=2`, `PACE_SPIN_US=1500` | kapalı | Bananza'nın 60/30/20 FPS kipini kare zamanlamasıyla yönlendirir |
| `DK_DYN=1` | kapalı | Bananza dinamik kare hızı yaması, bellekte (aşağıda) |
| `DK_DYN_MIN_FPS=<n>` | 40 | yamanın alt sınırı (30 daha kötü ölçüldü) |
| `FG_TIMED=1` | kapalı | FSR3 ara kareleri ekranda kalacakları ana göre karıştırılır |
| `OSD=1` | kapalı | RTSS'de oyun FPS'i, ekran FPS'i (kare üretme çarpanıyla) ve hız |
| `LIGHT_DRAW`, `LIGHT_UNIFORM`, `UBO_*`, `SYNC_*`, `VERTEX_*`, `RESET_*`, ... (60 kadar) | **açık** | GPU iş parçacığında çizim başı kısayollar; `=0` 2.0.0'ın kod yolu, çoğu `=2` ile kendini denetleyip günlüğe yazar |
| `HLE_INDIRECT`, `INDIRECT_BATCH`, `PBFLUSH_WINDOW_KIB` | **açık** | Bananza arazisi: dolaylı çizimler ana makinenin GPU'sunda ve birleşik; GPU'nun yazdığı parametreler 256 KiB'lık pencerelerle boşaltılır |
| `RX_RT_FIT=3` | kapalı | reaktif boşaltmayla: Bananza'nın açılışı çizim başına kopya yerine tek görüntüye çizer, sırayı çizim geçişinin içindeki bir bariyer sağlar (deneysel; `=1` bunun yerine çizim geçişini bitirir, daha yavaş; ikisi de `barrier_feedback_loops=true` ister) |
| `FLUSH_FIRST_READ`, `READ_LATCH_AGE`, `RX_ONE_SUBMIT`, `RX_COALESCE`, `RX_PITCH_FORCED` | kapalı | reaktif boşaltma deneyleri; kazanç ölçülmedi, önerilmez |
| `RX_MASKED_TARGETS` | kapalı | **bilinen kusur, açmayın** |
| `RX_COALESCE_LOOSE` | kapalı | yalnız tanı (reaktif boşaltmanın sıralamasını zayıflatır): oyun için değil |

## Donkey Kong Bananza dinamik kare hızı (DK-DYN)

`EDEN_NX2_DK_DYN=1` oyuna özel bir yama uygular: Bananza'nın ana modülü yüklenirken oyunun kare
hızı denetleyicisinin birkaç komutu **bellekte** değiştirilir; böylece oyun 30'a düşmek yerine 40
ile 60 FPS arasında her hızda çalışabilir ve saati gerçek kare süresiyle ilerler (oyunun hızı 24
FPS'e dek doğru kalır). ZendNevs çatalının `dk_dynamic_fps` yamasının (v2.0.5) aktarımıdır. Diske hiçbir şey
yazılmaz, hiçbir oyun dosyası değişmez; anahtar boşken hiçbir modüle dokunulmaz. Yalnız Bananza'ya
(başlık 040074A01BF12000) ve yalnız beklenen komutlar bulunursa uygulanır. `DK_DYN_MIN_FPS=<1-60>`
40 FPS sınırını değiştirir.

Programda varsayılan kapalıdır; `start-recommended.bat` açar (başındaki `DK_DYN=1`). Açıkken
oyunun kare süresi bilerek değişkendir (40 ile 60 arası, 2'lik adımlarla). Yama uygulanınca
`PRESENT_INTERVAL_WORD` ve `MODE_PACE` devre dışı kalır, sunumlar her vblank'e göre zamanlanır; 40 FPS'in altında oyun yine
kendi 30 FPS kuralına iner.

## Kare üretme (isteğe bağlı)

PokeDinner-Eglantine 2.0.0'da kare üretme zaten var; NX2Perf `FG_TIMED`'i ekler ve çarpanı
göstergede gösterir. İki türü var:

- **FSR3** (yerleşik, varsayılan tür). Bizim ölçtüğümüz budur.
- **LSFG** (Lossless Scaling kare üretmesi). Ücretli *Lossless Scaling* uygulamasının
  `Lossless.dll`'ini ister. **NX2Perf bunu içermez**; bu türü isterseniz kendi kopyanızı kullanıcı
  klasöründeki `lossless\` klasörüne koyun.

eden-cli'de FSR3 kare üretmeyi açmak için programı kapatın ve `config\sdl2-config.ini`'de
`[Renderer]` altına şu satırları ekleyin (`\default=false` satırları eden-cli'nin değeri okumasını
sağlar):

```ini
frame_gen\default=false
frame_gen=true
frame_gen_target_rate\default=false
frame_gen_target_rate=120
```

`frame_gen_target_rate`, kare üretmenin hedeflediği kare hızıdır (120 Hz ve üstü ekranda 120
kullanıyoruz; `0` ise `frame_gen_multiplier`'ı, 2-4, kullanır). `frame_gen_type=0` LSFG'yi seçer.

## Reaktif boşaltma

`use_reactive_flushing` (`[Renderer]`), GPU'nun yazdığı veriyi oyun okuduğunda oyunun belleğine geri
kopyalar. **Programın masaüstündeki varsayılanı açıktır.** Bizim deneme kurulumumuzda **kapalıydı**
ve aşağıdaki önerilen sayılar böyle ölçüldü. Bizim makinede, daha eski derlemelerle ve kare
üretme kapalıyken, açmak geniş alanlarda FPS'i %13-23 artırdı, oyunun açılışında %23-30 düşürdü. `sdl2-config.ini`'nize bakın:

```ini
use_reactive_flushing\default=false
use_reactive_flushing=false
```

bizim önerilen kurulumumuza karşılık gelir; `=true` (ya da bu satırların hiç olmaması) reaktif
boşaltma açık demektir, `start-reactive-experimental.bat` bunun içindir. İkisini birden tutmak için
.ini'yi kopyalayın (ör. aynı klasöre `sdl2-config-reactive.ini`), kopyayı değiştirin ve tam yolunu
bir başlatıcının `CONFIG=` satırına yazın.

Bu sürüm reaktif boşaltma için anahtarlar ekler (hepsi varsayılan kapalı;
`nx2_switches_NX2PERF.txt`, "reactive flushing"). `RX_RT_FIT` açılışta bulunan bedeli hedefler:
orada Bananza 2112x1179'luk bir görüntüye çizerken aynı belleği 2095x1179'luk bir doku olarak
okuyor; reaktif boşaltmayla her çizim birini ötekine kopyalıyordu. Anahtarla ikisi tek görüntü
olur ve onu okuyan bir çizim, ona yazan çizimlerden sonra gelmelidir: `=1` böyle bir çizimden önce
çizim geçişini bitirir; `=3` bunun yerine çizim geçişinin içine bir bariyer yazar (renk eki
yazımları parça gölgelendirici okumalarından önce, bölgesel), çizim geçişi sürer. İkisi de
`[Renderer]` altında `barrier_feedback_loops=true` ister (o olmadan anahtar bir şey yapmaz).

5 Kasım'da ölçüldü (DK-DYN açık, kare üretme kapalı, LTO + PGO derlemeleri, kol başına iki koşunun
oyun FPS'i; her satır tek bir derlemede reaktif kapalı ile açığı karşılaştırır):

| Kollar (satır başına tek derleme) | Açılış | Geniş alan |
|---|---|---|
| reaktif kapalı -> açık + `RX_RT_FIT=1` | 43,4 / 42,9 -> 32,9 / 33,8 (−%23) | 46,4 / 46,8 -> 52,6 / 52,9 (+%13) |
| reaktif kapalı -> açık + `RX_RT_FIT=3` | 42,7 / 40,8 -> 38,4 / 39,0 (−%7) | 45,4 / 46,2 -> 52,4 / 52,5 (+%15) |

`RX_RT_FIT=3` açılışta `=1`'den açıkça iyi (+%16), geniş alanda aynı. Reaktif kapalıya göre ise
sahneye bağlı bir takas: açılışta biraz yavaş, geniş alanlarda hızlı. Bu yüzden yayında oyun için
iki başlatıcı var: `start-recommended.bat` (reaktif kapalı) ve `start-reactive-experimental.bat`
(açık, `RX_RT_FIT=3` ile). **Deneysel:** reaktif boşaltma kare üretmeyle birlikte **hiç
ölçülmedi** ve `RX_RT_FIT=3`'ün görüntüsüne henüz gözle bakılmadı (çizim geçişinin içindeki
bariyeri, bir parçanın kendi çizdiği pikseli okumasını kapsar; Vulkan belirtiminin geri besleme
döngüleri için tanımladığı budur). Yanlış görüntü görürseniz başlatıcıda `RX_RT_FIT=1` yapın ya
da önerilen başlatıcıyı kullanın. `RX_MASKED_TARGETS`'ı (bilinen kusur: bir doku kopyalaması
maskeyi yok sayıyor) ve `RX_COALESCE_LOOSE`'u (yalnız tanı) açmayın.

## Ölçülen sonuçlar

Yukarıdaki makinede, yalnız Donkey Kong Bananza ile ölçüldü. FPS oyunun kendi kare hızıdır (aksi
yazmıyorsa kare üretme kapalı); RTSS'nin paylaşımlı belleğinden ya da emülatör günlüğünden okundu.
"Aynı derleme" satırları tek programda tek bir şeyi değiştirir. Sahneler: **açılış** (kaydın
başladığı alan), **geniş alan** (ilk bölümün geniş bir görüntüsü), **uzak bakış** (bölüme elle
uzaktan bakış; kamera koşudan koşuya birebir aynı değil).

| Değişiklik | Kollar (A -> B) | Sahne, koşu | Sonuç |
|---|---|---|---|
| Hepsi, PokeDinner 2.0.0 -> NX2Perf | 2.0.0'ın `eglantine.exe`'si (Qt; el kipi, GPU doğruluğu Fast, 1x) / bu çatalın 3-5 Kasım `eden-cli` derlemeleri (çoğu TV kipi) | 2.0.0: elle uzak bakış, 25 sn (31 Ekim). Çatal: açılış ve geniş alanın otomatik koşuları | 2.0.0: 17,3 FPS (kare süresi medyanı 58 ms). Çatal: sınırsız 42-53 FPS (3-4 Kasım); DK-DYN açık, kare üretme kapalı 39-46 FPS (5 Kasım, `b5a994e3eb` commit'inin derlemesi). **Denetimli bir A/B değil**: program, kip, sahneler ve koşular farklı, 2.0.0'ın sahnesi en ağırı; aynı uzak bakışta çatalın 31 Ekim - 1 Kasım derlemeleri 19,7 ve ~30,8 FPS ölçtü (aşağıdaki satırlar). Yalnız kaba bir gösterge |
| GPU'nun yazdığı parametrelerin boşaltma penceresi | aynı derleme (31 Ekim, öteki değişikliklerin çoğundan önce), `PBFLUSH_WINDOW_KIB=0` -> varsayılan | uzak bakış, kol başına 2 x 25 sn | 18,0 -> 19,7 FPS (+%9; `HLE_INDIRECT`'ten önce ölçüldü, asıl `HLE_INDIRECT=0` iken önemli) |
| Arazi dolaylı çizimleri ana makinenin GPU'sunda | aynı derleme (1 Kasım), `HLE_INDIRECT=0` -> varsayılan | elle uzak bakış, kol başına 2-4 koşu | ~20,8 -> ~30,8 FPS (~+%48; kamera birebir aynı değil) |
| Arka aşamalar (kesim a) | `BACK_STAGES=0` -> `1` | açılış, 2 tur x 2 x 25 sn | +%8,7 / +%7,3 |
| Çizim başı kısayollar (ilk takım, 2 Kasım; hepsi değil) | kapalı -> açık | açılış, aynı koşular (2. turda bir örnek dışarıda) | +%7,5 / +%6,8 |
| Acquire peek | `ACQUIRE_PEEK=0` -> `1` | açılış, aynı koşular | +%1,4 / +%3,9 |
| Profil güdümlü iyileştirme (PGO) | LTO derlemesi -> LTO + PGO | açılış, aynı koşular | +%21,5 / +%15,8 (3 Kasım derlemesi ve profili; bu sürümün programının kendi profili var, böyle ölçülmedi) |
| Ön tarafın makro JIT süzgeci | kapalı -> `JIT_SAME=1` + `FRONT_MACRO_LEAN=1` | açılış, 2 tur x 2 | 39,6 -> 42,1 FPS (+%6) |
| Reaktif boşaltma | kapalı -> açık | geniş alan / açılış, gece otomatik koşuları (3-4 Kasım derlemeleri) | +%13-23 / −%23-30 |
| `RX_RT_FIT=1`'li reaktif boşaltma | aynı derleme, reaktif kapalı -> açık + `RX_RT_FIT=1` (DK-DYN açık) | açılış / geniş alan, kol başına 2 koşu (5 Kasım) | −%23 / +%13 |
| `RX_RT_FIT=3`'lü reaktif boşaltma | aynı derleme, reaktif kapalı -> açık + `RX_RT_FIT=3` (DK-DYN açık) | açılış / geniş alan, kol başına 2 koşu (5 Kasım) | −%7 / +%15 (`=3`, `=1`'e göre: açılışta +%16, geniş alanda aynı) |
| `DK_DYN_MIN_FPS=30` | boş (40) -> 30 | açılış ve geniş alan, her sahnede kol başına 2 koşu | −%4 / −%5 (açılışta FPS aralıkları örtüşüyor; 30'a yakın karelerin artması daha açık işaret): önerilmez |

DK-DYN ve 120'yi hedefleyen FSR3 kare üretmesiyle (önerilen kurulumun daha eski bir derlemesi),
3,6 dakikalık olağan bir oyun oturumunun günlüğünde oyun FPS'i medyan 46 (en düşük 35), ekranda
saniyede medyan 119 kare (en düşük 97), oyun hızı medyan %99,8 (en düşük %93) çıktı. Bu bir oturumdur, kıyas ölçümü
değildir.

## Bilinen sorunlar

- **Yalnız komut satırı programı.** Qt programı (`eglantine.exe`) bu sürümde yok: derleme ve PGO
  profili `eden-cli` içindir. Menüleri için 2.0.0'ın `eglantine.exe`'sini kullanmayı sürdürün;
  orada değiştirilen ayarlar eden-cli'ye geçmez (`qt-config.ini` ile `sdl2-config.ini` ayrı).
- **Parlak kenarlı siyah bloklar** bazı yerlerde görünüyor. Kare üretmeli bir oturumda ve ondan
  önce kare üretmesiz görüldüler; olası neden kare üretme değil, bu oyunun daha eski bir çizim
  sorunu (upstream için de bildirildi), ama kesinleşmedi. Görürseniz dünyada yerinde kalıp
  kalmadığına bakın, kare üretmesiz ve `start-safe.bat` ile deneyin, nerede olduğunu bildirin.
- **Reaktif boşaltma** (varsayılan açık) oyunun açılışında FPS'e mal oluyor (yukarıda); yeni
  anahtarları kare üretmeyle ölçülmedi.
- **AMD ve Intel ekran kartları denenmedi**; başka oyunlar da. Bananza anahtarları
  (`PRESENT_INTERVAL_WORD`, `MODE_PACE`, `DK_DYN`) yalnız Bananza içindir.
- Gösterge RTSS'nin paylaşımlı belleğine yazar; program zorla kapatılırsa (Görev Yöneticisi)
  yazı RTSS yeniden başlatılana dek ekranda kalabilir.
- Profil güdümlü iyileştirmeyle derlenmiş program, eğitildiği yollarda (Bananza, önerilen
  anahtarlar) en hızlıdır. Başka ayarlar da çalışır, belki biraz yavaş.

## Yapay zekâ beyanı

Bu çatalın kodunun, commit mesajlarının ve bu belgelerin çoğunu bir yapay zekâ kodlama asistanı
(Anthropic'in Claude Opus 5.5'i) insan bakımcının yönetiminde yazdı. Neyin deneneceğine bakımcı
karar verdi, derlemeleri o oynayıp denedi ve sürümün sorumluluğu ondadır. Yukarıdaki ölçümler
bakımcının bilgisayarında, çoğu otomatik koşularla alındı. Asistanla yapılan her commit bunu
`Assisted-by:` satırıyla belirtir; Eden projesinin yapay zekâ politikası (`docs/policies/AI.md`)
bunu ister. Bu çatal kendi başına yayımlanır, upstream'e gönderilmez. İçine alınan upstream
commit'leri yazarlarıyla değişiklik günlüğünde listelenir.

## Lisans

Eden ve PokeDinner-Eglantine gibi NX2Perf de **GNU Genel Kamu Lisansı v3.0 ya da sonrası** ile
lisanslıdır (`LICENSE.txt`). PokeDinner-Eglantine 2.0.0'ın değiştirilmiş bir sürümüdür; neyin ne
zaman değiştiği `NOTICE_NX2PERF.txt`'de (yayın zip'lerinde) ve `CHANGELOG_NX2PERF.md`'dedir. Bu sürümün eksiksiz karşılık
gelen kaynak kodu `<KAYNAK_URL>`'de ve programın yanındaki `-source.zip`'tedir; üçüncü taraf
kütüphanelerin nasıl indirildiği `BUILDING_NX2PERF.md`'de. Üçüncü taraf bileşenler kendi
lisanslarını korur: bildirimleri ve lisans metinleri `THIRD_PARTY_NOTICES_NX2PERF.txt` ve
`LICENSES\` içinde; kaynaktan derlenen kütüphanelerin kaynağı `-deps-source.zip`'te (önceden
derlenmişler, FFmpeg, OpenSSL, SDL3 ve sirit için bildirimler upstream kaynaklarını gösterir).

Nintendo Switch ve Donkey Kong Bananza, Nintendo'nun ticari markalarıdır. Bu projenin Nintendo
ile bir bağı yoktur. Yalnız sahip olduğunuz oyunlarla kullanın.
