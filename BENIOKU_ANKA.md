# Anka

**Anka — Windows için, performansa odaklı bir Nintendo Switch 2 emülatörü.**

**Anka by EddizEge.** Nintendo, Eden projesi ya da Franseven ile bağı yoktur; sorunları onlara
değil, bu deponun Issues sayfasına bildirin: https://github.com/EddizEge/Anka/issues

**Ad:** Anka, adını Türk mitolojisinin Zümrüdüankası'ndan, küllerinden yeniden doğan kuştan alır:
kapanan yuzu'nun küllerinden doğan bir emülatör.

Oyun, anahtar ve firmware pakette yoktur, sağlanmayacak: yalnız yasal olarak sahip olduğunuz
oyunlarla ve kendi cihazınızdan aldığınız anahtar ve firmware ile kullanın.

**Yol haritası:** bundan sonra bütün güncellemeler tek emülatöre, Anka'ya gelir. Sıradaki sürüm:
Zelda desteği ve oyuna özel hazır ayarlar. Tarih verilmedi.

## Bu sürüm

**Anka 1.0.0**, PokeDinner-Eglantine 2.0.0 tabanlıdır ve mevcut bir PokeDinner-Eglantine 2.0.0
klasörüne açılır. İki program içerir:

- **`anka.exe`**, menülü program: 2.0.0'ın `eglantine.exe`'siyle aynı Qt ön yüzü (oyun listesi,
  menüler, ayarlar); Anka ayarları içinde, ayarlar penceresinde bir **Anka** sekmesi var. Kolay
  yol budur.
- **`anka-cli.exe`**, menüsüz (komut satırı / SDL) program; başlatıcılar (`start-*.bat`) onu
  ölçtüğümüz ayarlarla açar. Bütün ölçümlerimiz bununla yapıldı (o zamanki adı eden-cli idi).

2.0.0'ın kendi `eglantine.exe` ve `eden-cli.exe`'si ayrı dosyalardır ve eskisi gibi kalır.
Eden, yuzu, Sudachi projeleriyle ya da Franseven'le bir bağı yoktur. Sorunlarını onlara
bildirmeyin; Anka'nın sorun izleyicisini kullanın: https://github.com/EddizEge/Anka/issues

- Taban: PokeDinner-a-Eglantine-Fork 2.0.0, commit `70ecb39ef0`
  (https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork)
- Bu sürüm: paketteki `VERSION_ANKA.txt` (etiket, commit, yapılandırma tarihi).
- Kaynak: https://github.com/EddizEge/Anka, dal `anka`, etiket `anka-v1.0.0` (programla aynı
  commit; GPLv3, bkz. "Lisans").
- Değişiklikler: `CHANGELOG_ANKA.md` (İngilizce). Bütün anahtarlar: `nx2_switches_ANKA.txt`
  (kaynakta `dist/nx2_switches.txt`).
- English: `README_ANKA.md`.

**Ölçümler: yalnız Donkey Kong Bananza, tek makine.** Genel iyileştirmeler her oyunda çalışır ama
yalnız Bananza'da ölçüldü; başka oyunlar için ölçülmüş bir hız ya da denenmiş bir uyumluluk iddiası
yoktur. Aşağıdaki bütün sayılar Ryzen 7 5700X
(8 çekirdek), 64 GB RAM, NVIDIA GeForce RTX 4060 Ti ve 1440p 170 Hz ekranlı bir makinede Donkey
Kong Bananza menüsüz programla ve başlatıcılarıyla oynanırken alındı. `anka.exe` aynı emülatör
kodunu aynı ayarlarla çalıştırır ama ayrıca ölçülmedi. Bananza'ya özel ayarlar başka oyunlarda
kendiliğinden kapalı kalır. Başka oyunlar, başka işlemciler, AMD ya da Intel ekran kartları
denenmedi.

## Gerekenler

- Windows 10 ya da 11, 64 bit.
- **Zorunlu:** çalışan bir **PokeDinner-Eglantine 2.0.0** kurulumu. Anka'nın zip'i o klasöre,
  `eglantine.exe`'nin yanına açılır ve `anka.exe` o klasörün Qt DLL'lerini kullanır (zip'te Qt
  yoktur). Kendi anahtarlarınız, firmware'iniz ve oyun dökümünüz de 2.0.0 için kurulduğu gibi
  orada olmalı; Anka bunların hiçbirini sağlamaz, sağlamayacak.
- **Microsoft Visual C++ 2015-2022 x64** çalışma zamanı (2.0.0 da ister). `anka-cli.exe`,
  2.0.0'ın `eden-cli.exe`'siyle birebir aynı DLL'leri kullanır. `anka.exe` 2.0.0 klasöründeki
  Qt 6.8.3 DLL'lerini ve eklentilerini kullanır (2.0.0'ın derlendiği Qt sürümü): **pakette Qt
  yoktur**, bu yüzden menülü program yalnız bir 2.0.0 klasörünün içinde açılır. Başka bir şey
  kurmak gerekmez.
- Güncel **Vulkan** sürücülü bir ekran kartı (2.0.0'daki gibi). Yalnız NVIDIA RTX 4060 Ti denendi.
- x86-64 işlemci. Derleme AVX2 istemez (AVX2 kodu yalnız Opus ses kodlayıcısında var ve işlemciyi
  çalışırken denetliyor), ama yalnız Ryzen 7 5700X denendi. Değişikliklerin çoğu GPU iş
  parçacığını hızlı bir masaüstü işlemcide hızlandırır; yavaş işlemcide kazanç daha az olur.
- Çok çekirdekli CPU öykünmesi açık (varsayılan). Kare zamanlama anahtarları bunu ister.
- İsteğe bağlı: ekranda FPS / hız göstergesi için RivaTuner Statistics Server (RTSS).

## İndirme ve denetim

Yayında beş zip ve `SHA256SUMS.txt` var. Oynamak için yalnız `Anka-1.0.0-win64.zip` gerekir;
`Anka-1.0.0-pdb.zip` hata ayıklama sembolleridir (çökme raporları için), `Anka-1.0.0-source.zip`,
`Anka-1.0.0-deps-source.zip` ve `Anka-1.0.0-prebuilt-source.zip` kaynak koddur (bkz. "Lisans").
İndirmeyi denetlemek için Git Bash'te `sha256sum -c SHA256SUMS.txt --ignore-missing`, ya da komut
isteminde `certutil -hashfile <zip> SHA256` yazıp dosyadakiyle karşılaştırın. Program imzasızdır:
Windows SmartScreen ilk açılışta uyarabilir.

## Kurulum

Bu resmî olmayan, deneysel bir derlemedir ve hiçbir garantisi yoktur (`NOTICE_ANKA.txt`).
2.0.0'ın kayıtlarını kullanır (aşağıda): **ilk açılıştan önce kullanıcı klasöründeki `nand`
klasörünü (kayıtlarınız) yedekleyin.**

1. `Anka-1.0.0-win64.zip`'i PokeDinner-Eglantine 2.0.0 klasörüne, `eglantine.exe`'nin yanına
   açın. Hiçbir dosyanın üstüne yazılmaz: yeni programların kendi adları var, 2.0.0 eskisi gibi
   çalışmaya devam eder. Qt DLL'leri o klasörden gelir, zip'te yoktur.
2. **`anka.exe`**'yi açın (pencere başlığı "Anka by EddizEge", ardından oyun / derleme bilgisi). 2.0.0'ın
   `eglantine.exe`'siyle aynı kullanıcı klasörünü ve aynı `qt-config.ini`'yi kullanır; oyun
   listeniz (2.0.0'dakiyle aynı oyunlar), kumandanız ve ayarlarınız zaten oradadır.
3. Ayarlar: **Emulation > Configure... > Anka**. İlk açılışta **Preset** kutusunda
   **Recommended** seçilidir: ölçtüğümüz kurulum. Anka ilk açılışta reaktif boşaltmayı
   (*Reactive Flushing*) da `qt-config.ini`'de kendiliğinden kapatır (öteki ayarlarla birlikte
   kaydedilir; dosya ortak olduğu için 2.0.0'ın `eglantine.exe`'si de onu kapalı görür). Oyununuzu
   her zamanki gibi listeden açın.

### Anka sekmesi

Sekmenin üstünde bir **Preset** (hazır ayar) kutusu var; dört seçenek, her birinin altında tek
satırlık açıklaması:

- **Recommended** (önerilen; ilk açılışta seçili): sekiz ayarın hepsi açık, *Reactive Flushing*
  (Graphics > Advanced) kapalı. Ölçtüğümüz kurulum; `start-recommended.bat` ile aynı. İlk
  açılışta Anka reaktif boşaltmayı kendisi kapatır; Recommended'ı sonradan kutudan seçmek de onu
  kapatır (yalnız oyun çalışmıyorken; çalışıyorsa sekme onu sonra Graphics > Advanced'de
  kapatmanızı söyler).
- **Recommended + Reactive Flushing**: sekizi açık, reaktif boşaltma açık, onun gerektirdiği
  *barrier feedback loops* açık (ikisi de Graphics > Advanced) ve `EDEN_NX2_RX_RT_FIT=3`.
  Deneysel: bizim makinede oyunun açılışında yaklaşık %7 yavaş, geniş alanlarda yaklaşık %15
  hızlı. `start-reactive-experimental.bat` ile aynı.
- **Safe** (güvenli): sekizi kapalı, Anka'nın varsayılanı açık tuttuğu GPU kısayolları da kapalı
  (`EDEN_NX2_LIGHT_DRAW=0` ve `start-safe.bat`'ın listesindeki ötekiler, toplam 61 anahtar `0`),
  her oyunda. `start-safe.bat`'ın anahtar listesiyle aynı. Grafik ayarlarına dokunmaz (reaktif
  boşaltma, *barrier feedback loops* olduğu gibi kalır); Anka'nın düzeltmeleri ve anahtarı
  olmayan değişiklikleri de içeride kalır.
- **Custom** (özel): bir kutuyu elle değiştirdiğinizde kendiliğinden seçilir.

Recommended yalnız Donkey Kong Bananza'da ölçüldü. Başka bir oyunda görüntü ya da hız sorunu
olursa önce Safe'i deneyin ve oyunun adıyla bildirin: https://github.com/EddizEge/Anka/issues

Altında iki grup var:

| Ayar | Koyduğu anahtarlar (`EDEN_NX2_...`) |
|---|---|
| **All games** (bütün oyunlar) | |
| Split GPU thread (bölünmüş GPU iş parçacığı) | `BACK_STAGES=1`, `BACK_CUTS=a` |
| Acquire peek | `ACQUIRE_PEEK=1` |
| Same-value filter in the macro JIT | `JIT_SAME=1` |
| Lean front macro path | `FRONT_MACRO_LEAN=1` |
| Timed frame generation | `FG_TIMED=1` (yalnız kare üretme açıkken etkili) |
| FPS overlay in RivaTuner | `OSD=1` (RTSS çalışıyorsa) |
| **Donkey Kong Bananza only** (başka oyunlarda kapalı) | |
| Dynamic frame rate | `DK_DYN=1` |
| Frame pacing | `PRESENT_INTERVAL_WORD=12:16:4`, `MODE_PACE=2`, `PACE_SPIN_US=1500` |

Bunlar `start-recommended.bat`'ın koyduğu anahtarlardır ("Başlatıcılar", "Başlıca anahtarlar").
Program onları bir oyun açılmadan hemen önce kendi ortamına koyar. İki Bananza ayarı yalnız oyunun
başlık numarası 040074A01BF12000 olduğunda konur (dinamik kare hızı yamasının denetlediği);
başka her oyunda kapalı kalırlar.

**Ne zaman geçerli olur:** anahtarlar oturum başına bir kez okunur. Ayarlar oturumda açtığınız
ilk oyunda geçerli olur; ilk oyundan önce soru sorulmaz, ayarlar doğrudan ona uygulanır. Bu
oturumda bir oyun açıldıktan sonra ayarları **OK** ya da **Apply** ile değiştirirseniz Anka sorar:
"Restart Anka now to apply?" **Restart Now** Anka'yı kapatıp yeniden açar; bir oyun çalışıyorsa
o oyunla (kaydedilmemiş oyun ilerlemesi kaybolur), çalışmıyorsa aynı komut satırıyla. **Later**
değişikliği sonraki açılışa bırakır. Oturumun ilk oyunundan başka anahtarlar isteyen bir oyun
açarsanız (başka bir oyundan sonra Bananza ya da tersi) yine sorar: **Restart** o oyunla yeniden
açar, **Start Anyway** oturumun anahtarlarıyla açar, **Cancel** vazgeçer.

**Dışarıda konan anahtar önceliklidir:** ortamınızda, `anka.exe`'yi açan kendi `.bat`'ınızda ya da
bir `nx2_switches.txt`'de (programın yanında ya da `%APPDATA%\eden`'de) konan anahtara Anka asla
dokunmaz. Anahtarlarının hepsi dışarıda konmuş kutu gri görünür; yalnız bir kısmı konmuşsa (ör.
kare zamanlamasının üç anahtarından biri) kutu ötekileri yine koyar. İpucu dışarıda konan
değişkenleri değerleriyle verir.

**2.0.0 ile ortak:** iki menülü program aynı `qt-config.ini`'yi kullanır; birinde değiştirdiğiniz
ayar (reaktif boşaltma da) ötekinde de geçerlidir. Yalnız `[Anka]` grubu yenidir; 2.0.0'ın
`eglantine.exe`'si ona dokunmaz.

**Kaldırma:** zip'in eklediği dosyaları silin (`anka.exe`, `anka-cli.exe`, üç `start-*.bat`,
`anka-config.ps1`, `*_ANKA.*` dosyaları, `LICENSE.txt`, `LICENSES\`,
`Games\PUT-YOUR-GAMES-HERE.txt`; 2.0.0'ın zip'inde bunların hiçbiri yok). **`Games`'te kendi oyun
dosyalarınız durur:** klasörü silmeden önce onları taşıyın. `qt-config.ini`'deki `[Anka]` grubu,
başlatıcıların ayar dosyaları (`config\sdl2-config.ini`, `config\sdl2-config-reactive.ini`) ve
gölgelendirici önbellekleri kullanıcı klasöründe kalır; durmalarında sakınca yok.

### İkinci yol: anka-cli ve başlatıcılar

1. Oyun dosyanızı (`.nsp`, `.xci`, `.nsz` ya da `.xcz`) yeni `Games` klasörüne koyun (alt
   klasörde de olabilir) ya da başlatıcının üstüne sürükleyin.
2. `start-recommended.bat`'a çift tıklayın.

**İlk açılışta** başlatıcı ayarları hazırlar. Anka, 2.0.0 ile aynı kullanıcı klasörünü kullanır
(taşınabilir kurulumda programın yanındaki `user\`, değilse `%APPDATA%\eden`): aynı anahtarlar,
firmware ve kayıtlar. anka-cli ayarlarını oradaki `config\sdl2-config.ini`'de tutar; Qt
programlarının `qt-config.ini`'sinden ayrıdır. `sdl2-config.ini` henüz yoksa başlatıcı onu
`qt-config.ini`'nin kopyası olarak oluşturur; böylece menülü programda ayarladığınız kumanda ve
ayarlar gelir. Kopyada reaktif boşaltmayı da kapatır (ölçülen kurulum, bkz. "Reaktif boşaltma").
Var olan bir `sdl2-config.ini`'yi de, `qt-config.ini`'yi de asla değiştirmez. Düzenlemeyi
başlatıcıların yanındaki `anka-config.ps1` yapar: küçük, okunur bir PowerShell betiği; o yoksa
başlatıcılar bu adımı atlayıp oyunu açar. İki dosya da yoksa başlatıcı bir iletiyle durur:
`eglantine.exe`'yi ya da `anka.exe`'yi bir kez açıp kumandanızı orada ayarlayın, sonra başlatıcıyı
yeniden açın. `Games`'te birden çok oyun varsa başlatıcı onları listeler ve numara sorar.
Başlatıcıların iletileri `[Anka]` ile başlar.

Oyun tam ekran açılır; anka-cli'nin menüsü yoktur. **Alt+F4 ile çıkın.**

## Başlatıcılar

| Başlatıcı | Ne zaman |
|---|---|
| `start-recommended.bat` | **Önerilen.** Ölçtüğümüz en hızlı ayarlar (Donkey Kong Bananza). |
| `start-reactive-experimental.bat` | **Deneysel.** Reaktif boşaltma açık: bizim makinede oyunun açılışında yaklaşık %7 yavaş, geniş alanlarda yaklaşık %15 hızlı. Görüntü hatası görürseniz önerilene dönün. |
| `start-safe.bat` | **Güvenli.** Anka'nın varsayılanı açık kısayol anahtarları `0`, oynanışı etkileyen isteğe bağlı anahtarlar boş; anahtarı olmayan değişiklikler içeride kalır. Sorun ararken ve Bananza dışındaki oyunlar için. |

Başlatıcı şu sırayla açar: üstüne sürüklenen oyun dosyası, `GAME=` satırı, ya da yanındaki
`Games` klasöründeki oyun (birden çoksa hangisi olduğunu sorar). Bir satırı değiştirmek için
`.bat`'ı metin düzenleyicide açın (sağ tık, Düzenle). Her başlatıcının başında değiştirilmek için
konmuş satırlar var:

| Satır | Anlamı |
|---|---|
| `GAME=` | açılacak oyun (sürüklenen dosya önceliklidir; bu satır `Games`'ten önce gelir) |
| `CONFIG=` | bu başlatıcı için başka bir `sdl2-config` .ini; boş = kendiliğinden (Kurulum; reaktif başlatıcı `sdl2-config-reactive.ini`'yi kullanır) |
| `FULLSCREEN=-f` | tam ekran; boş = pencere |
| `DK_DYN=1` | Bananza dinamik kare hızı yaması (aşağıda); `0` = kapalı |
| `OSD=1` | RTSS ile FPS / hız göstergesi; `0` = kapalı |

(`DK_DYN=` ve `OSD=` yalnız önerilen ve reaktif başlatıcılarda var.) Oyunun yolunda `&`, `^`, `,`,
`;` ya da `=` olup boşluk yoksa Windows sürüklenen dosyayı bozuk iletir: oyunu `Games`'e koyun ya
da yolu `GAME=` satırına yazın. `CONFIG=` satırındaki dosya yoksa başlatıcı bir iletiyle durur
(anka-cli sessizce varsayılan ayarlarla yeni bir dosya yazardı); emülatör hatayla kapanırsa
pencere açık kalır. Başlatıcıları yayın zip'inden alın: Windows satır sonları (CRLF) gerekir.

- **`start-recommended.bat`** (önerilen): bizim oynadığımız ayarlar (Donkey Kong Bananza).
  GPU iş parçacığının arka tarafı aşamalı (`BACK_STAGES=1`, `BACK_CUTS=a`), `ACQUIRE_PEEK=1`,
  `JIT_SAME=1`, `FRONT_MACRO_LEAN=1`; Bananza'nın kendi 60/30/20 FPS seçimi doğru hızda izlenir
  (`PRESENT_INTERVAL_WORD=12:16:4`, `MODE_PACE=2`, `PACE_SPIN_US=1500`); dinamik kare hızı yaması
  (`DK_DYN=1`; yama uygulanınca yukarıdaki iki hız satırı devre dışı kalır: `DK_DYN=0` iken ya
  da yama uygulanmazsa etkilidirler); zamanlı FSR3 ara kareleri (`FG_TIMED=1`, yalnız kare
  üretme açıkken etkili); RTSS göstergesi (`OSD=1`). Bananza'ya özel satırlar işaretli; başka
  oyunlar için güvenli başlatıcıyı kullanın ya da o satırları silin. Reaktif boşaltma
  **kapalıyken** ölçüldü: oluşturduğu `sdl2-config.ini`'de onu kapatır, var olan dosyada açıksa
  yalnız bir not yazar ("Reaktif boşaltma").
- **`start-safe.bat`** (güvenli): Anka'nın varsayılanı açık kısayol anahtarları `0`, oynanışı
  etkileyen isteğe bağlı anahtarlar boş (`AUTO_INPUT_FILE`, `VI2_REPLY` gibi birkaç tanı anahtarı
  boşaltılmaz: bunları sistem ortamınızda tanımlamayın). Sorun ararken (bir sorunun bu
  anahtarlardan gelip gelmediğini anlamak için) ve Bananza dışındaki oyunlar için. Anahtarı
  olmayan değişiklikler (değişiklik günlüğündeki doğruluk düzeltmeleri ve upstream'den alınanlar)
  yine içeridedir.
- **`start-reactive-experimental.bat`** (reaktif, DENEYSEL; önerilenin yanında ikinci seçenek):
  reaktif boşaltma açıkken önerilen ayarlar, artı `RX_RT_FIT=3` (Bananza'nın açılışındaki çizim
  hedefi için çizim başına bir kopya yerine tek görüntü). Bizim makinede açılışı geniş alanlara
  değişir: reaktif kapalı aynı derlemeye göre oyunun açılışında FPS −%7, geniş alanda +%15. Henüz
  denenmedi: kare üretmeyle birlikte çalışması ve `RX_RT_FIT=3`'ün görüntüsü. Reaktif boşaltma bir
  anahtar değil, bir ayardır: `CONFIG=` boşken bu başlatıcı `config\sdl2-config-reactive.ini`'yi
  kullanır; onu bir kez, `sdl2-config.ini`'nin reaktif boşaltma ve `barrier_feedback_loops` açık
  kopyası olarak oluşturur (aşağıda "Reaktif boşaltma"). Reaktif boşaltması kapalı bir `CONFIG=`
  dosyasıyla da `RX_RT_FIT=3` etkilidir; bu bileşim ölçülmedi.

anka-cli anahtarları yalnız ortamından (başlatıcılardan) okur; Anka sekmesini okumaz.
`nx2_switches_ANKA.txt` her anahtarı varsayılanı ve ne yaptığıyla listeler. (`nx2_switches.txt`'yi
yalnız menülü programlar okur, 2.0.0'ınki ve `anka.exe`; paket onu değiştirmez.)

## Başlıca anahtarlar

Hepsi `EDEN_NX2_<AD>` adlı ortam değişkenleridir. Tam liste (yaklaşık 230 yeni ad, çoğu tanı ya da
ince ayar) `nx2_switches_ANKA.txt`'de. "Varsayılan", başlatıcısız `anka-cli.exe`'dir; `anka.exe`
önerilenleri Anka sekmesinden koyar.

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
| `RX_RT_FIT=3` | kapalı | reaktif boşaltma için (onsuz da etkili, öyle ölçülmedi): Bananza'nın açılışı çizim başına kopya yerine tek görüntüye çizer, sırayı çizim geçişinin içindeki bir bariyer sağlar (deneysel; `=1` bunun yerine çizim geçişini bitirir, daha yavaş; ikisi de `barrier_feedback_loops=true` ister) |
| `FLUSH_FIRST_READ`, `READ_LATCH_AGE`, `RX_ONE_SUBMIT`, `RX_COALESCE`, `RX_PITCH_FORCED` | kapalı | reaktif boşaltma deneyleri; kazanç ölçülmedi, önerilmez |
| `RX_MASKED_TARGETS` | kapalı | **bilinen kusur, açmayın** |
| `RX_COALESCE_LOOSE` | kapalı | yalnız tanı (reaktif boşaltmanın sıralamasını zayıflatır): oyun için değil |

## Donkey Kong Bananza dinamik kare hızı (DK-DYN)

`EDEN_NX2_DK_DYN=1` oyuna özel bir yama uygular: Bananza'nın ana modülü yüklenirken oyunun kare
hızı denetleyicisinin birkaç komutu **bellekte** değiştirilir; böylece oyun 30'a düşmek yerine 40
ile 60 FPS arasında her hızda çalışabilir ve saati gerçek kare süresiyle ilerler (oyunun hızı 24
FPS'e dek doğru kalır). ZendNevs çatalının `dk_dynamic_fps` yamasının (v2.0.5) aktarımıdır. Diske
hiçbir şey yazılmaz, hiçbir oyun dosyası değişmez; anahtar boşken hiçbir modüle dokunulmaz. Yalnız
Bananza'ya (başlık 040074A01BF12000) ve yalnız beklenen komutlar bulunursa uygulanır.
`DK_DYN_MIN_FPS=<1-60>` 40 FPS sınırını değiştirir.

anka-cli'de varsayılan kapalıdır; `start-recommended.bat` açar (başındaki `DK_DYN=1`), Anka
sekmesi de (Dynamic frame rate; Recommended'da açık, yalnız Bananza). Açıkken oyunun kare süresi
bilerek değişkendir (40 ile 60 arası, 2'lik adımlarla). Yama uygulanınca `PRESENT_INTERVAL_WORD`
ve `MODE_PACE` devre dışı kalır, sunumlar her vblank'e göre zamanlanır; 40 FPS'in altında oyun
yine kendi 30 FPS kuralına iner.

## Kare üretme (isteğe bağlı)

PokeDinner-Eglantine 2.0.0'da kare üretme zaten var; Anka `FG_TIMED`'i ekler ve çarpanı
göstergede gösterir. İki türü var:

- **FSR3** (yerleşik, varsayılan tür). Bizim ölçtüğümüz budur.
- **LSFG** (Lossless Scaling kare üretmesi). Ücretli *Lossless Scaling* uygulamasının
  `Lossless.dll`'ini ister. **Anka bunu içermez**; bu türü isterseniz kendi kopyanızı kullanıcı
  klasöründeki `lossless\` klasörüne koyun.

`anka.exe`'de kare üretme, 2.0.0'daki gibi grafik ayarlarındadır. anka-cli'de FSR3 kare üretmeyi
açmak için programı kapatın ve `config\sdl2-config.ini`'de `[Renderer]` altına şu satırları ekleyin
(`\default=false` satırları anka-cli'nin değeri okumasını sağlar):

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
üretme kapalıyken, açmak geniş alanlarda FPS'i %13-23 artırdı, oyunun açılışında %23-30 düşürdü.
`sdl2-config.ini`'nize bakın:

```ini
use_reactive_flushing\default=false
use_reactive_flushing=false
```

bizim önerilen kurulumumuza karşılık gelir; `=true` (ya da bu satırların hiç olmaması) reaktif
boşaltma açık demektir. Menülü programda Graphics > Advanced altındaki *Enable Reactive
Flushing*'tir. Anka ilk açılışında onu `qt-config.ini`'de kendiliğinden kapatır; Anka sekmesinde
Recommended onu kapatır, Recommended + Reactive Flushing onu ve gerektirdiği *barrier feedback
loops*'u açar (`qt-config.ini` 2.0.0'ın `eglantine.exe`'siyle ortaktır). anka-cli için bunu
başlatıcılar halleder. `start-recommended.bat` ilk açılışta oluşturduğu `sdl2-config.ini`'ye bu iki satırı
yazar; var olan dosyayı asla değiştirmez, orada reaktif boşaltma açıksa yalnız bir not yazar
(ölçülen kurulum için iki satırı program kapalıyken elle değiştirin).
`start-reactive-experimental.bat` kendi kopyasını kullanır: `config\sdl2-config-reactive.ini`, bir
kez `sdl2-config.ini`'den `use_reactive_flushing=true` ve `barrier_feedback_loops=true` ile
oluşturulur. Sonradan iki dosyadan birinde değiştirilen ayar ötekine geçmez:
`sdl2-config-reactive.ini`'yi silerseniz reaktif başlatıcı onu yeniden oluşturur.

Bu sürüm reaktif boşaltma için anahtarlar ekler (hepsi varsayılan kapalı;
`nx2_switches_ANKA.txt`, "reactive flushing"). `RX_RT_FIT` açılışta bulunan bedeli hedefler:
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
iki başlatıcı (ve iki hazır ayar) var: `start-recommended.bat` (reaktif kapalı) ve
`start-reactive-experimental.bat` (açık, `RX_RT_FIT=3` ile). **Deneysel:** reaktif boşaltma kare
üretmeyle birlikte **hiç ölçülmedi** ve `RX_RT_FIT=3`'ün görüntüsüne henüz gözle bakılmadı (çizim
geçişinin içindeki bariyeri, bir parçanın kendi çizdiği pikseli okumasını kapsar; Vulkan
belirtiminin geri besleme döngüleri için tanımladığı budur). Yanlış görüntü görürseniz başlatıcıda
`RX_RT_FIT=1` yapın ya da önerilen başlatıcıyı kullanın. `RX_MASKED_TARGETS`'ı (bilinen kusur: bir
doku kopyalaması maskeyi yok sayıyor) ve `RX_COALESCE_LOOSE`'u (yalnız tanı) açmayın.

## Ölçülen sonuçlar

Yukarıdaki makinede, yalnız Donkey Kong Bananza ile, menüsüz programla (o zamanki adıyla
`eden-cli`) ölçüldü. FPS oyunun kendi kare hızıdır (aksi yazmıyorsa kare üretme kapalı); RTSS'nin
paylaşımlı belleğinden ya da emülatör günlüğünden okundu. "Aynı derleme" satırları tek programda
tek bir şeyi değiştirir. Sahneler: **açılış** (kaydın başladığı alan), **geniş alan** (ilk bölümün
geniş bir görüntüsü), **uzak bakış** (bölüme elle uzaktan bakış; kamera koşudan koşuya birebir
aynı değil).

| Değişiklik | Kollar (A -> B) | Sahne, koşu | Sonuç |
|---|---|---|---|
| Hepsi, PokeDinner 2.0.0 -> Anka | 2.0.0'ın `eglantine.exe`'si (Qt; el kipi, GPU doğruluğu Fast, 1x) / bu çatalın 3-5 Kasım `eden-cli` derlemeleri (çoğu TV kipi) | 2.0.0: elle uzak bakış, 25 sn (31 Ekim). Çatal: açılış ve geniş alanın otomatik koşuları | 2.0.0: 17,3 FPS (kare süresi medyanı 58 ms). Çatal: sınırsız 42-53 FPS (3-4 Kasım); DK-DYN açık, kare üretme kapalı 39-46 FPS (5 Kasım, `2d424d6efd` commit'inin derlemesi). **Denetimli bir A/B değil**: program, kip, sahneler ve koşular farklı, 2.0.0'ın sahnesi en ağırı; aynı uzak bakışta çatalın 31 Ekim - 1 Kasım derlemeleri 19,7 ve ~30,8 FPS ölçtü (aşağıdaki satırlar). Yalnız kaba bir gösterge |
| GPU'nun yazdığı parametrelerin boşaltma penceresi | aynı derleme (31 Ekim, öteki değişikliklerin çoğundan önce), `PBFLUSH_WINDOW_KIB=0` -> varsayılan | uzak bakış, kol başına 2 x 25 sn | 18,0 -> 19,7 FPS (+%9; `HLE_INDIRECT`'ten önce ölçüldü, asıl `HLE_INDIRECT=0` iken önemli) |
| Arazi dolaylı çizimleri ana makinenin GPU'sunda | aynı derleme (1 Kasım), `HLE_INDIRECT=0` -> varsayılan | elle uzak bakış, kol başına 2-4 koşu | ~20,8 -> ~30,8 FPS (~+%48; kamera birebir aynı değil) |
| Arka aşamalar (kesim a) | `BACK_STAGES=0` -> `1` | açılış, 2 tur x 2 x 25 sn | +%8,7 / +%7,3 |
| Çizim başı kısayollar (ilk takım, 2 Kasım; hepsi değil) | kapalı -> açık | açılış, aynı koşular (2. turda bir örnek dışarıda) | +%7,5 / +%6,8 |
| Acquire peek | `ACQUIRE_PEEK=0` -> `1` | açılış, aynı koşular | +%1,4 / +%3,9 |
| Profil güdümlü iyileştirme (PGO) | LTO derlemesi -> LTO + PGO | açılış, aynı koşular | +%21,5 / +%15,8 (3 Kasım derlemesi ve profili; bu sürümün programı o profili yeniden kullanır, bkz. "Bilinen sorunlar"; böyle ölçülmedi) |
| Ön tarafın makro JIT süzgeci | kapalı -> `JIT_SAME=1` + `FRONT_MACRO_LEAN=1` | açılış, 2 tur x 2 | 39,6 -> 42,1 FPS (+%6) |
| Reaktif boşaltma | kapalı -> açık | geniş alan / açılış, gece otomatik koşuları (3-4 Kasım derlemeleri) | +%13-23 / −%23-30 |
| `RX_RT_FIT=1`'li reaktif boşaltma | aynı derleme, reaktif kapalı -> açık + `RX_RT_FIT=1` (DK-DYN açık) | açılış / geniş alan, kol başına 2 koşu (5 Kasım) | −%23 / +%13 |
| `RX_RT_FIT=3`'lü reaktif boşaltma | aynı derleme, reaktif kapalı -> açık + `RX_RT_FIT=3` (DK-DYN açık) | açılış / geniş alan, kol başına 2 koşu (5 Kasım) | −%7 / +%15 (`=3`, `=1`'e göre: açılışta +%16, geniş alanda aynı) |
| `DK_DYN_MIN_FPS=30` | boş (40) -> 30 | açılış ve geniş alan, her sahnede kol başına 2 koşu | −%4 / −%5 (açılışta FPS aralıkları örtüşüyor; 30'a yakın karelerin artması daha açık işaret): önerilmez |

DK-DYN ve 120'yi hedefleyen FSR3 kare üretmesiyle (önerilen kurulumun daha eski bir derlemesi),
3,6 dakikalık olağan bir oyun oturumunun günlüğünde oyun FPS'i medyan 46 (en düşük 35), ekranda
saniyede medyan 119 kare (en düşük 97), oyun hızı medyan %99,8 (en düşük %93) çıktı. Bu bir
oturumdur, kıyas ölçümü değildir.

## Bilinen sorunlar

- **Menülü program ölçülmedi.** `anka.exe`, `anka-cli.exe` ile aynı emülatör kodunu aynı
  anahtarlarla ve aynı PGO profiliyle çalıştırır; ama buradaki bütün sayılar menüsüz programdan
  (o zamanki adıyla eden-cli). Bir 2.0.0 klasörünün Qt 6.8.3 DLL'lerine ihtiyaç duyar.
- Menülü programda Home Menu uygulamasından açılan bir oyun oturumun ayarlarıyla çalışır (soru
  sorulmaz). **Restart** oyunu normal yoldan açar (oyunun kendi ayarları, ilk programı).
- Başlatıcılar `qt-config.ini`'yi ilk açılışta bir kez kopyalar; menülü programlarda sonradan
  değiştirilen ayarlar anka-cli'ye (`sdl2-config.ini`) geçmez. Yeniden kopyalatmak için
  `sdl2-config.ini`'nin (ve `sdl2-config-reactive.ini`'nin) adını değiştirip bir başlatıcıyı açın.
- **Parlak kenarlı siyah bloklar** bazı yerlerde görünüyor. Kare üretmeli bir oturumda ve ondan
  önce kare üretmesiz görüldüler; olası neden kare üretme değil, bu oyunun daha eski bir çizim
  sorunu (upstream için de bildirildi), ama kesinleşmedi. Görürseniz dünyada yerinde kalıp
  kalmadığına bakın, kare üretmesiz ve `start-safe.bat` (ya da Safe hazır ayarı) ile deneyin,
  nerede olduğunu bildirin.
- **Reaktif boşaltma** (programın varsayılanı; önerilen başlatıcı onu yalnız kendi oluşturduğu
  `sdl2-config.ini`'de kapatır) oyunun açılışında FPS'e mal oluyor (yukarıda); yeni anahtarları
  kare üretmeyle ölçülmedi.
- **AMD ve Intel ekran kartları denenmedi**; başka oyunlar da. Bananza anahtarları
  (`PRESENT_INTERVAL_WORD`, `MODE_PACE`, `DK_DYN`) yalnız Bananza içindir.
- Gösterge RTSS'nin paylaşımlı belleğine yazar; program zorla kapatılırsa (Görev Yöneticisi)
  yazı RTSS yeniden başlatılana dek ekranda kalabilir.
- Profil güdümlü iyileştirmeyle derlenmiş program, eğitildiği yollarda en hızlıdır. Bu sürüm
  daha önceki bir eğitim koşusunun profilini yeniden kullanır (commit `c8bbd32fe9`, 3 Kasım:
  o günün anahtarlarıyla Bananza; `DK_DYN`, `FG_TIMED` ve `RX_RT_FIT`'ten önce, reaktif boşaltma
  kapalı). Yeni ve başka yollar da çalışır, belki biraz yavaş. İki program da bu profili kullanır;
  menülü programın kendi Qt kodu profil almaz.

## Yapay zekâ beyanı

Bu çatalın kodunun, commit mesajlarının ve bu belgelerin çoğunu bir yapay zekâ kodlama asistanı
(Anthropic'in Claude Opus 5.5'i) insan bakımcının yönetiminde yazdı. Neyin deneneceğine bakımcı
karar verdi, derlemeleri o oynayıp denedi ve sürümün sorumluluğu ondadır. Yukarıdaki ölçümler
bakımcının bilgisayarında, çoğu otomatik koşularla alındı. Asistanla yapılan her commit (iki düz
git birleştirmesi dışında) bunu `Assisted-by:` satırıyla belirtir; Eden projesinin yapay zekâ
politikası (`docs/policies/AI.md`) bunu ister. Bu çatal kendi başına yayımlanır, upstream'e
gönderilmez. İçine alınan upstream commit'leri yazarlarıyla değişiklik günlüğünde listelenir.

## Emeği geçenler / Üzerine kurulduğu projeler

Anka; Eden, Eglantine (Franseven'in PokeDinner çatalı), yuzu ve Sudachi üzerine kuruludur; onların
telif ve emek kayıtları korunur. Bu projelerin resmî bir sürümü değildir. Menülü programın Hakkında
penceresi de bunu söyler: "A Nintendo Switch 2 emulator for Windows, focused on performance" ve
altta "Built on Eden and Eglantine (Franseven), yuzu and Sudachi".

## Lisans

Eden ve PokeDinner-Eglantine gibi Anka da **GNU Genel Kamu Lisansı v3.0 ya da sonrası** ile
lisanslıdır (`LICENSE.txt`). PokeDinner-Eglantine 2.0.0'ın değiştirilmiş bir sürümüdür; neyin ne
zaman değiştiği `NOTICE_ANKA.txt`'de (yayın zip'lerinde) ve `CHANGELOG_ANKA.md`'dedir. Programın
kaynak kodu (yayın commit'i) https://github.com/EddizEge/Anka adresinde ve programın yanındaki
`Anka-1.0.0-source.zip`'tedir; üçüncü taraf kütüphanelerin nasıl indirildiği `BUILDING_ANKA.md`'de.
Üçüncü taraf bileşenler kendi lisanslarını korur: bildirimleri ve lisans metinleri
`THIRD_PARTY_NOTICES_ANKA.txt` ve `LICENSES\` içinde. Menülü program Qt 6.8.3'e dinamik bağlanır
(Qt Base ve Qt SVG LGPL-3.0, Qt Charts GPL-3.0 ile): Qt DLL'leri 2.0.0 klasörünüzdekilerdir, bu
pakette yoktur; onları kendi derlediğiniz Qt 6.8.3 ile değiştirebilirsiniz. QuaZip (LGPL-2.1,
statik bağlama istisnasıyla) içine derlenmiştir. Kaynaktan derlenen kütüphanelerin, QuaZip dahil,
kaynağı `Anka-1.0.0-deps-source.zip`'te. Dört kütüphane önceden derlenmiş paketlerden bağlanır:
FFmpeg, OpenSSL, SDL3 ve sirit. Kaynakları `Anka-1.0.0-prebuilt-source.zip`'tedir: tam
commit'lerin upstream kaynak arşivleri, önceden derlenmiş paketleri üreten crueter-ci derleme
betikleri (configure / CMake seçenekleriyle), FFmpeg'e derlemeden önce uygulanan yama ve sirit'in
SPIRV-Headers dahil tam kaynağı (o zip'teki `README_PREBUILT_SOURCES.txt`). FFmpeg
(LGPL-2.1-or-later, yamalı) statik bağlıdır: değiştirilmiş bir FFmpeg kullanmak için onu
`BUILDING_ANKA.md`'deki gibi derleyip programı `-source.zip`'ten ona karşı bağlayın. OpenSSL
paketi ayrıca Mozilla'nın CA sertifika listesini (MPL-2.0) programa derler; sirit'in deposunda
BSD-3-Clause dosya başlıklarının yanında GPL-3.0 ve AGPL-3.0 lisans dosyaları da var: bkz.
`THIRD_PARTY_NOTICES_ANKA.txt`. Yayın bağlamasında kullanılan PGO profili kaynak değil eğitim
verisidir ve yayınlanmaz; onsuz yapılan derleme aynı çalışır ama bayt bayt aynı olmaz.

Anka logosu (küllerinden yükselen, alev kanatlı ve alev tepelikli bir anka kuşu) bu projenin
kendi tasarımıdır.

Nintendo Switch ve Donkey Kong Bananza, Nintendo'nun ticari markalarıdır. Bu projenin Nintendo
ile bir bağı yoktur. Yalnız sahip olduğunuz oyunlarla kullanın.
