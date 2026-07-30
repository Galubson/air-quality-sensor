# Czujnik jakości powietrza - ESP32

Firmware dla XIAO ESP32S3 (lub ESP32-DevKitV1 do prototypowania) z czujnikami
SPS30, BME280 i SHT40. Działa w pełni samodzielnie (panel WWW + sterowanie
przekaźnikiem), a integracja z Home Assistant jest opcjonalną warstwą przez MQTT.

## Dwie płytki, jeden projekt

`platformio.ini` ma teraz dwa środowiska na stałe:

- `devkit_v1` - to, na czym teraz prototypujesz. Ustawione jako domyślne
  (`default_envs`), więc zwykłe Upload w VS Code / `pio run` buduje na nie.
- `xiao_esp32s3` - płytka docelowa. Żeby wgrać na nią bez zmiany pliku:
  `pio run -e xiao_esp32s3 -t upload`. Gdy ostatecznie przesiądziesz się na
  XIAO, zmień w `platformio.ini` samą linijkę `default_envs = xiao_esp32s3`.

Piny I2C są zdefiniowane osobno dla każdego środowiska (`I2C_SDA_PIN` /
`I2C_SCL_PIN` w `build_flags`) - `main.cpp` sam dobierze właściwe w zależności
od tego, które środowisko budujesz. Nic nie trzeba zmieniać w kodzie.

## Podłączenie (I2C, wspólna magistrala)

| Czujnik | Piny DevKitV1 | Piny XIAO ESP32S3 | Adres I2C |
|---|---|---|---|
| BME280  | SDA=GPIO21, SCL=GPIO22 | SDA=GPIO5, SCL=GPIO6 | 0x76 lub 0x77 |
| SHT40   | SDA=GPIO21, SCL=GPIO22 | SDA=GPIO5, SCL=GPIO6 | 0x44 |
| SPS30   | SDA=GPIO21, SCL=GPIO22 | SDA=GPIO5, SCL=GPIO6 | 0x69 (tryb I2C - zworka/piny SEL wg instrukcji Sensirion) |

Wszystkie trzy urządzenia wiszą na tej samej magistrali I2C - różne adresy,
więc nie kolidują. Pamiętaj o podciągnięciu SDA/SCL rezystorami 4.7kΩ do 3.3V,
jeśli płytki czujników same tego nie robią (Adafruit BME280/SHT40 zwykle mają
wbudowane pull-upy - przy trzech urządzeniach na magistrali może być ich za
dużo/za mało, sprawdź to jako pierwszy krok w razie problemów z komunikacją).

## Pierwsze uruchomienie

1. Wgraj firmware (`pio run -t upload` w PlatformIO albo przez VS Code).
2. Po starcie urządzenie tworzy sieć WiFi `AirQuality-Setup-XXXX`.
3. Połącz się z nią telefonem/laptopem - powinno pojawić się okno konfiguracji
   (captive portal). Jeśli nie pojawi się automatycznie, wejdź na `192.168.4.1`.
4. Wybierz swoją sieć WiFi, podaj hasło, adres IP Shelly Wall Plug S oraz
   próg PM2.5 (albo zostaw wartość domyślną 35 µg/m³) - to tylko wartości
   startowe, wszystko da się później zmienić w panelu WWW.
5. Po zapisaniu urządzenie łączy się z siecią domową i restartuje. Panel WWW
   jest dostępny pod `http://czujnik-powietrza.local/` (działa
   automatycznie w większości sieci domowych - nie trzeba znać adresu IP).

   Jeśli ten adres nie działa (rzadkie, głównie starsze Windowsy bez
   zainstalowanego Bonjoura): sprawdź na routerze listę podłączonych
   urządzeń - powinno tam być widoczne jako „czujnik-powietrza” zamiast
   anonimowego „ESP32”, więc łatwo je znaleźć nawet bez mDNS. Adres IP z tej
   listy też zadziała, po prostu jest mniej wygodny (może się zmienić po
   restarcie routera, w przeciwieństwie do adresu `.local`).

## Dlaczego urządzenie samo łączy się z zapamiętaną siecią

To normalne zachowanie ESP32/WiFiManager - moduł WiFi w ESP32 zapisuje dane
ostatnio połączonej sieci we własnej pamięci flash (niezależnie od reszty
konfiguracji tego projektu) i przy starcie zawsze najpierw próbuje się z nią
połączyć. Dopiero jeśli się nie uda, uruchamia tryb AP + captive portal.

Żeby to zmienić, w panelu WWW (karta „Sieć WiFi”) są dwie opcje:

- Połącz z inną siecią - wpisujesz SSID i hasło nowej sieci, urządzenie
  łączy się z nią od razu i się restartuje.
- Zapomnij sieć i uruchom portal konfiguracyjny - kasuje zapisaną sieć
  i wymusza tryb AP (`AirQuality-Setup-XXXX`), tak jak przy pierwszym uruchomieniu.

## Sterowanie przekaźnikiem Shelly

Panel komunikuje się z Shelly Wall Plug S przez jego natywne RPC API
(`/rpc/Switch.Set?id=0&on=true|false` do przełączania, `/rpc/Switch.GetStatus?id=0`
do odczytu stanu - urządzenie odpytuje go co 15 sekund, więc panel pokazuje
prawdziwy stan nawet jeśli ktoś przełączył wtyczkę ręcznie albo z aplikacji Shelly).

W karcie „Przekaźnik” w panelu WWW:
- przełącznik Tryb automatyczny - włączony: przekaźnik steruje się sam wg
  progu PM2.5 i histerezy - powyżej progu przekaźnik się wyłącza (żeby
  rekuperacja nie wciągała złego powietrza z zewnątrz do domu), a po
  poprawie jakości powietrza (z zapasem histerezy) włącza się z powrotem;
  wyłączony: automatyka jest wstrzymana
- przyciski Włącz/Wyłącz - zawsze aktywne, niezależnie od trybu. Kliknięcie
  w trakcie trybu automatycznego samo przełącza na tryb ręczny (żeby automatyka
  nie cofnęła zmiany przy najbliższym sprawdzeniu progu)
- pole Adres IP Shelly - akceptuje zarówno samo IP (`192.168.30.34`), jak
  i pełny adres (`http://192.168.30.34/`) - oba formaty działają identycznie
- pola Próg PM2.5 / Histereza

## Integracja z Home Assistant (opcjonalnie)

Ważne: adres brokera MQTT to NIE to samo co adres Home Assistant. To był
najpewniej powód, dla którego nic się nie pojawiało - wpisanie samego IP HA
nic nie da, jeśli pod tym adresem i portem 1883 nie odpowiada żaden broker MQTT.

Żeby to zadziałało, potrzebujesz:

1. Działającego brokera MQTT - najprościej dodatek „Mosquitto broker”
   z poziomu Home Assistant (Ustawienia → Dodatki → sklep z dodatkami →
   szukaj „Mosquitto”). Po instalacji i uruchomieniu broker nasłuchuje
   zwykle na tym samym IP co HA, na porcie 1883.
2. Konta użytkownika do logowania - domyślny dodatek Mosquitto zwykle
   nie pozwala na połączenia anonimowe. Utwórz użytkownika w
   Ustawienia → Osoby → Użytkownicy w HA i wpisz jego login/hasło w panelu
   WWW urządzenia (karta „Home Assistant”, pola Użytkownik/Hasło - nowość,
   wcześniej ich nie było).
3. Włączonej integracji MQTT w HA - Ustawienia → Urządzenia i usługi →
   Dodaj integrację → MQTT (jeśli używasz dodatku Mosquitto, HA zwykle
   proponuje to automatycznie).

Panel WWW pokazuje teraz rzeczywisty status połączenia (karta „Home
Assistant”, wiersz „Status”) z czytelnym opisem błędu zamiast ciszy - np.
„Brak autoryzacji” oznacza zły login/hasło, „Timeout połączenia” zwykle zły
adres IP albo broker w ogóle nie działa.

Ważna pułapka: samo udane połączenie (widoczne w logach Mosquitto jako
„New client connected”) nie gwarantuje, że encje pojawią się w HA - biblioteka
PubSubClient domyślnie ogranicza wiadomość MQTT do 256 bajtów, a wiadomości
discovery (z pełnym opisem urządzenia) mają ok. 350-400 bajtów. Bez zwiększenia
bufora (`mqttClient.setBufferSize(512)` w `setup()`) `publish()` po cichu
zwraca `false` i nic nie dociera, mimo zdrowego połączenia. To już naprawione
w kodzie, ale warto o tym wiedzieć, gdyby w przyszłości payloady dalej urosły.

Jeśli wszystko poprawne, urządzenie po połączeniu samo opublikuje konfigurację
discovery - encje pojawią się w HA automatycznie, bez ręcznego dodawania.
Jeśli pole brokera zostawisz puste, urządzenie po prostu nigdy nie próbuje
łączyć się z MQTT - panel WWW i sterowanie przekaźnikiem działają identycznie.

## Tryb ciemny

Panel WWW dopasowuje się automatycznie do ustawień systemu/przeglądarki
(`prefers-color-scheme`), a dodatkowo w prawym górnym rogu jest przycisk do
ręcznego przełączenia - wybór zapamiętuje się w przeglądarce (localStorage)
niezależnie od ustawień systemowych.

## Instalacja bez PlatformIO (dla finalnego użytkownika)

Folder `web-installer/` zawiera gotową stronę do wgrywania firmware wprost
z przeglądarki (Chrome/Edge) przez USB - bez instalowania PlatformIO,
Arduino IDE czy czegokolwiek innego po stronie osoby, która ma zaprogramować
płytkę. Wymaga jednorazowego przygotowania z Twojej strony (zbudowanie i
scalenie plików binarnych) - dokładne kroki w `web-installer/JAK_PRZYGOTOWAC.md`.

## Fizyczny reset (utrata dostępu do WiFi i do AP)

Obie płytki (DevKitV1 i XIAO ESP32S3) mają wbudowany przycisk BOOT
podpięty pod GPIO0 - nic nie trzeba dolutowywać. Dwa progi przytrzymania:

- 3 sekundy (puszczony przed 8s) - kasuje tylko ochronę hasłem panelu
  (wraca do stanu „bez hasła”, login resetuje się na `admin`). WiFi, adres
  Shelly, progi PM i dane MQTT zostają bez zmian. To odpowiedź na „zapomniałem
  hasła do panelu” - nie trzeba konfigurować wszystkiego od nowa.
- 8 sekund - pełny reset fabryczny: kasuje zapisaną sieć WiFi oraz
  wszystkie ustawienia aplikacji (adres Shelly, progi PM, dane logowania
  MQTT, hasło panelu) i uruchamia urządzenie ponownie w trybie AP + portal
  konfiguracyjny - dokładnie tak, jakby było świeżo wgrane po raz pierwszy.

Działa zarówno na starcie (blokujące sprawdzenie zanim urządzenie w ogóle
spróbuje łączyć się z WiFi - na wypadek gdyby coś się zawiesiło), jak i w
trakcie normalnej pracy (sprawdzane w pętli głównej). Serial Monitor
(115200 baud) pokazuje na bieżąco, który próg został osiągnięty.

Uwaga: przycisk BOOT na XIAO jest fizycznie bardzo mały (myślany do
wgrywania firmware, nie codziennego użytku) - przy projektowaniu obudowy
warto dolutować równolegle wygodniejszy przycisk między GPIO0 a GND.

## Brakujące czujniki nie blokują urządzenia

Przed inicjalizacją każdego czujnika firmware najpierw sprawdza jednym krótkim
zapytaniem I2C, czy cokolwiek w ogóle odpowiada pod jego adresem. Jeśli nie -
w ogóle nie wywołuje biblioteki tego czujnika (część bibliotek potrafi się
zapętlić we własnych próbach ponawiania, gdy sprzętu fizycznie nie ma).

Panel WWW startuje normalnie niezależnie od tego, ile czujników jest
podłączonych. Karta „Odczyty” pokazuje `-` zamiast liczby dla brakujących
pomiarów oraz żółty baner z listą tego, czego nie wykryto, np.
„Nie wykryto: SPS30 (PM), SHT40”. Przydatne przy budowie/testowaniu, gdy
czujniki są podłączane po kolei, a nie wszystkie naraz.

## Rzeczy do sprawdzenia przed pierwszą kompilacją

- API biblioteki SPS30 - zweryfikowane na podstawie oficjalnego przykładu
  z repozytorium `paulvha/sps30` (`sps30.begin(I2C_COMMS)` / `.probe()` /
  `.start()` / `.GetValues(&val)`, pola `val.MassPM1/MassPM2/MassPM4/MassPM10`).
  Jeśli mimo to dostaniesz błędy kompilacji w tej sekcji (biblioteka bywa
  aktualizowana), zerknij do przykładu dołączonego do zainstalowanej wersji
  (Examples -> sps30 -> Example1_sps30_BasicReadings) i daj mi znać dokładny
  błąd - dopasuję kod.
- Nazwy bibliotek w `platformio.ini` - rejestr PlatformIO czasem zmienia
  właścicieli/nazwy pakietów. Jeśli któraś się nie zainstaluje, wyszukaj ją
  ręcznie w zakładce Libraries po słowie kluczowym z komentarza obok.
- Histereza progu PM2.5 - domyślnie 5 µg/m³, żeby przekaźnik nie przełączał
  się co chwilę przy wahaniach odczytu w okolicy progu. Dostosuj do swoich
  potrzeb w panelu WWW.

## Odporność na utratę WiFi w trakcie pracy

Jeśli połączenie padnie (np. restart routera), urządzenie samo próbuje wrócić
do sieci co 30 sekund (`WiFi.reconnect()`). Jeśli po 5 minutach nadal nic
z tego, robi twardy restart jako ostateczność - czasem to jedyny sposób,
żeby wyprowadzić stos WiFi ESP32 z zawieszonego stanu. Dotyczy to tylko utraty
połączenia w locie - pierwsze łączenie przy starcie i ręczna zmiana sieci
z panelu to osobna, już wcześniej opisana ścieżka.

## Wygładzanie odczytów PM

SPS30 potrafi mocno skakać próbka do próbki. Odczyty PM1/PM2.5/PM4/PM10
przechodzą przez wykładniczą średnią ruchomą (EMA, współczynnik 0.3 w
`main.cpp`, stała `PM_EMA_ALPHA`) - zmniejsza to szum bez dużego opóźnienia
reakcji na realną zmianę. Pierwszy odczyt po starcie/wykryciu czujnika nie
jest wygładzany (ustawia wartość startową wprost), żeby nie czekać, aż
średnia "dojdzie" do prawdziwej wartości.

## Opcjonalne hasło do panelu

Domyślnie panel jest dostępny dla każdego w Twojej sieci domowej bez hasła -
to zwykle wystarczające zabezpieczenie w prywatnej sieci. Jeśli chcesz
dodatkową ochronę (np. sieć współdzielona z innymi osobami), włącz to w
karcie „Bezpieczeństwo panelu”. Ochrony nie da się włączyć bez ustawienia
hasła (blokada po stronie urządzenia, żeby nie dawać złudnego poczucia
bezpieczeństwa).

To własny formularz logowania (nie natywne okienko przeglądarki jak przy
zwykłym HTTP Basic Auth) - dzięki temu jest nad nim realna kontrola:

- sesja to ciasteczko bez trwałego zapisu - znika, gdy zamkniesz
  przeglądarkę (nie trzyma się „na zawsze” w tle)
- w karcie „Bezpieczeństwo panelu” jest przycisk „Wyloguj się teraz”
- restart urządzenia też czyści aktywną sesję - trzeba zalogować się od nowa

Jeśli zapomnisz hasła do panelu: przytrzymaj przycisk BOOT na płytce przez
3 sekundy (i puść przed 8s) - kasuje to tylko ochronę hasłem, WiFi i reszta
ustawień zostają nietknięte. Szczegóły w sekcji
[Fizyczny reset](#fizyczny-reset-utrata-dostępu-do-wifi-i-do-ap) poniżej.

## Aktualizacja firmware przez WiFi (OTA)

Nie trzeba za każdym razem podłączać kabla USB - po pierwszym wgraniu
(zwykłym, przez USB) kolejne aktualizacje można wysyłać przez sieć:

```
pio run -e xiao_esp32s3_ota -t upload
```

(dla DevKitV1: `pio run -e devkit_v1_ota -t upload`)

Zanim zaczniesz używać: w `main.cpp` (stała `OTA_PASSWORD`) i w
`platformio.ini` (obie linijki `upload_flags = --auth=...`) jest domyślne
hasło `zmien-to-haslo-123` - zmień je na własne, silne hasło w obu
miejscach na raz (muszą się zgadzać). Bez hasła każdy w Twojej sieci
mógłby wgrać dowolny kod na urządzenie - to poważniejsze ryzyko niż brak
hasła do panelu WWW, bo to pełna podmiana firmware, nie tylko odczyt/zmiana
ustawień.

Urządzenie musi już działać i być w sieci (adres `czujnik-powietrza.local`
używany jest jako cel uploadu). Jeśli coś pójdzie nie tak w trakcie
aktualizacji (np. zerwie się WiFi), urządzenie może wymagać ratunkowego
wgrania przez USB - dlatego zwykłe środowiska (`devkit_v1`, `xiao_esp32s3`)
zostają jako plan B, nic z nimi się nie zmieniło.

## Czego jeszcze nie ma (do dodania w kolejnym kroku, jeśli potrzebne)

Obecnie brak otwartych punktów - wszystko z wcześniejszej listy zostało
zaimplementowane i przetestowane na sprzęcie (w tym sterowanie Shelly Wall
Plug S przez `/rpc/Switch.Set`, które faktycznie włącza/wyłącza przekaźnik).