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

- **`start-recommended.bat`** (önerilen): bizim oynadığımız ayarlar (Donkey Kong Bananza).
  GPU iş parçacığının arka tarafı aşamalı (`BACK_STAGES=1`, `BACK_CUTS=a`), `ACQUIRE_PEEK=1`,
  `JIT_SAME=1`, `FRONT_MACRO_LEAN=1`; Bananza'nın kendi 60/30/20 FPS seçimi doğru hızda izlenir
  (`PRESENT_INTERVAL_WORD=12:16:4`, `MODE_PACE=2`, `PACE_SPIN_US=1500`); dinamik kare hızı yaması
  (`DK_DYN=1`); zamanlı FSR3 ara kareleri (`FG_TIMED=1`, yalnız kare üretme açıkken etkili); RTSS
  göstergesi (`OSD=1`). Bananza'ya özel satırlar işaretli; başka oyunlar için güvenli başlatıcıyı
  kullanın ya da o satırları silin.
- **`start-safe.bat`** (güvenli): bu çatalın eklediği bütün anahtarlar kapalı (varsayılanı açık
  kısayollar `0`, isteğe bağlı olanlar boş). Programın 2.0.0'a en yakın hâli; bir sorunun yeni
  anahtarlardan gelip gelmediğini anlamak için. Anahtarı olmayan değişiklikler (değişiklik
  günlüğündeki doğruluk düzeltmeleri ve upstream'den alınanlar) yine içeridedir.
- **`start-reactive-draft.bat`** (reaktif, TASLAK): reaktif boşaltma açıkken kullanılmak üzere
  önerilen ayarlar, artı onun bedelini azaltan anahtarlar (son listesi yayında belli olur).
  Reaktif boşaltma bir anahtar değil, bir ayardır: aşağıda "Reaktif boşaltma".

eden-cli anahtarları yalnız ortamından (başlatıcılardan) okur. `nx2_switches_NX2PERF.txt` her
anahtarı varsayılanı ve ne yaptığıyla listeler. (2.0.0'ın kendi `nx2_switches.txt`'sini yalnız Qt
programı okur; paket onu değiştirmez.)

## Başlıca anahtarlar

Hepsi `EDEN_NX2_<AD>` adlı ortam değişkenleridir. Tam liste (214 yeni ad, çoğu tanı ya da ince ayar)
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

## Donkey Kong Bananza dinamik kare hızı (DK-DYN)

`EDEN_NX2_DK_DYN=1` oyuna özel bir yama uygular: Bananza'nın ana modülü yüklenirken oyunun kare
hızı denetleyicisinin birkaç komutu **bellekte** değiştirilir; böylece oyun 30'a düşmek yerine 40
ile 60 FPS arasında her hızda çalışabilir ve saati gerçek kare süresiyle ilerler (oyunun hızı doğru
kalır). ZendNevs çatalının `dk_dynamic_fps` yamasının (v2.0.5) aktarımıdır. Diske hiçbir şey
yazılmaz, hiçbir oyun dosyası değişmez; anahtar boşken hiçbir modüle dokunulmaz. Yalnız Bananza'ya
(başlık 040074A01BF12000) ve yalnız beklenen komutlar bulunursa uygulanır. `DK_DYN_MIN_FPS=<1-60>`
40 FPS sınırını değiştirir.

Programda varsayılan kapalıdır; `start-recommended.bat` açar (başındaki `DK_DYN=1`). Açıkken
oyunun kare süresi bilerek değişkendir (40 ile 60 arası her hız); 40 FPS'in altında oyun yine
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
ve aşağıdaki önerilen sayılar böyle ölçüldü. Bizim makinede açmak geniş alanlarda FPS'i %13-20
artırdı, oyunun açılışında %28 düşürdü. `sdl2-config.ini`'nize bakın:

```ini
use_reactive_flushing\default=false
use_reactive_flushing=false
```

bizim önerilen kurulumumuza karşılık gelir; `=true` (ya da bu satırların hiç olmaması) reaktif
boşaltma açık demektir, `start-reactive-draft.bat` bunun içindir. İkisini birden tutmak için
.ini'yi kopyalayın (ör. aynı klasöre `sdl2-config-reactive.ini`), kopyayı değiştirin ve tam yolunu
bir başlatıcının `CONFIG=` satırına yazın.

## Ölçülen sonuçlar

Yukarıdaki makinede, yalnız Donkey Kong Bananza ile ölçüldü. FPS oyunun kendi kare hızıdır (aksi
yazmıyorsa kare üretme kapalı); RTSS'nin paylaşımlı belleğinden ya da emülatör günlüğünden okundu.
"Aynı derleme" satırları tek programda tek bir şeyi değiştirir. Sahneler: **açılış** (kaydın
başladığı alan), **geniş alan** (ilk bölümün geniş bir görüntüsü), **uzak bakış** (bölüme elle
uzaktan bakış; kamera koşudan koşuya birebir aynı değil).

| Değişiklik | Kollar (A -> B) | Sahne, koşu | Sonuç |
|---|---|---|---|
| Hepsi, PokeDinner 2.0.0 -> NX2Perf | 2.0.0'ın `eden-cli`'si / bu çatalın 3-5 Kasım derlemeleri | 2.0.0: elle uzak bakış, 25 sn (31 Ekim). Çatal: açılış ve geniş alanın otomatik koşuları | 2.0.0: 17,3 FPS (kare süresi medyanı 58 ms). Çatal: sınırsız 42-51 FPS (3-4 Kasım); DK-DYN açık, kare üretme kapalı 39-46 FPS (5 Kasım, bu sürümden bir önceki commit'in derlemesi). **Denetimli bir A/B değil**: sahneler ve koşular farklı; büyüklük mertebesi olarak okuyun |
| GPU'nun yazdığı parametrelerin boşaltma penceresi | aynı derleme, `PBFLUSH_WINDOW_KIB=0` -> varsayılan | uzak bakış, kol başına 2 x 25 sn | 18,0 -> 19,7 FPS (+%9) |
| Arazi dolaylı çizimleri ana makinenin GPU'sunda | aynı derleme, `HLE_INDIRECT=0` -> varsayılan | elle uzak bakış, kol başına 2-4 koşu | ~20,8 -> ~30,8 FPS (~+%48; kamera birebir aynı değil) |
| Arka aşamalar (kesim a) | `BACK_STAGES=0` -> `1` | açılış, 2 tur x 2 x 25 sn | +%8,7 / +%7,3 |
| Çizim başı kısayollar (b-hiz takımı) | kapalı -> açık | açılış, aynı koşular | +%7,5 / +%6,8 |
| Acquire peek | `ACQUIRE_PEEK=0` -> `1` | açılış, aynı koşular | +%1,4 / +%3,9 |
| Profil güdümlü iyileştirme (PGO) | LTO derlemesi -> LTO + PGO | açılış, aynı koşular | +%21,5 / +%15,8 |
| Ön tarafın makro JIT süzgeci | kapalı -> `JIT_SAME=1` + `FRONT_MACRO_LEAN=1` | açılış, 2 tur x 2 | 39,6 -> 42,1 FPS (+%6) |
| Reaktif boşaltma | kapalı -> açık | geniş alan / açılış, gece otomatik koşuları | +%13-20 / −%28 |
| `DK_DYN_MIN_FPS=30` | boş (40) -> 30 | açılış ve geniş alan, 4 + 4 koşu | −%4 / −%5, 30'a yakın kare daha sık: önerilmez |

DK-DYN ve 120'yi hedefleyen FSR3 kare üretmesiyle (önerilen kurulumun daha eski bir derlemesi),
3,6 dakikalık olağan bir oyun oturumunun günlüğünde oyun FPS'i medyan 46 (en düşük 35), ekranda
saniyede medyan 119 kare (en düşük 97), oyun hızı %99,8 çıktı. Bu bir oturumdur, kıyas ölçümü
değildir.

## Bilinen sorunlar

- **Yalnız komut satırı programı.** Qt programı (`eglantine.exe`) bu sürümde yok: derleme ve PGO
  profili `eden-cli` içindir. Menüleri için 2.0.0'ın `eglantine.exe`'sini kullanmayı sürdürün;
  orada değiştirilen ayarlar eden-cli'ye geçmez (`qt-config.ini` ile `sdl2-config.ini` ayrı).
- **Parlak kenarlı siyah bloklar** bazı yerlerde görünüyor. Kare üretmeli bir oturumda ve ondan
  önce kare üretmesiz görüldüler; olası neden kare üretme değil, bu oyunun daha eski bir çizim
  sorunu (upstream için de bildirildi), ama kesinleşmedi. Görürseniz dünyada yerinde kalıp
  kalmadığına bakın, kare üretmesiz ve `start-safe.bat` ile deneyin, nerede olduğunu bildirin.
- **Reaktif boşaltma** (varsayılan açık) oyunun açılışında FPS'e mal oluyor (yukarıda).
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
zaman değiştiği `NOTICE_NX2PERF.txt` ve `CHANGELOG_NX2PERF.md`'dedir. Bu sürümün eksiksiz karşılık
gelen kaynak kodu `<KAYNAK_URL>`'de ve programın yanındaki `-source.zip`'tedir; üçüncü taraf
kütüphanelerin nasıl indirildiği `BUILDING_NX2PERF.md`'de. Üçüncü taraf bileşenler kendi
lisanslarını korur (`LICENSES\`).

Nintendo Switch ve Donkey Kong Bananza, Nintendo'nun ticari markalarıdır. Bu projenin Nintendo
ile bir bağı yoktur. Yalnız sahip olduğunuz oyunlarla kullanın.
