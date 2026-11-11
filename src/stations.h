/* stations.h - lista stanica za panel. Ovo je jedino mesto koje menjas.
 * Sve stanice su proverene sa Pi-a (curl: 200/206 + audio/(mpeg|aac|aacp)).
 * HLS (.m3u8) stanice namerno nisu ovde - MPD ih ne moze bez ffmpeg-a.
 * Napomena: font na panelu je ASCII - izbegavaj nase dijakritike u imenima
 * (npr. "Sumadinac", ne "Šumadinac"), inace se prikazuju kao kvadrati.
 * 16.10.2026: suzeno na 20 stanica (Fox: 3 Naxi + TDI + jazz + internacionalne).
 */
#ifndef STATIONS_H
#define STATIONS_H

typedef struct {
    const char *name;
    const char *url;
} station_t;

static const station_t stations[] = {
    /* Naxi (izbor) */
    { "Naxi Radio",      "http://naxi128.streaming.rs:9150/" },
    { "Naxi 80-e",       "https://naxidigital-80s128ssl.streaming.rs:8042/;" },
    { "Naxi Cafe",       "https://naxidigital-cafe128ssl.streaming.rs:8022/;" },
    /* Ostale srpske (izbor) */
    { "TDI Radio",       "http://streaming.tdiradio.com:8000/tdiradio" },
    /* Jazz - contemporary + smooth (provereni 16.10.2026 sa Pi-a) */
    { "Jazz24",          "https://knkx-live-a.edge.audiocdn.com/6285_128k" },
    { "Swiss Jazz",      "http://stream.srg-ssr.ch/m/rsj/mp3_128" },
    { "FIP Jazz",        "http://icecast.radiofrance.fr/fipjazz-hifi.aac" },
    { "Jazz Radio FR",   "http://jazzradio.ice.infomaniak.ch/jazzradio-high.mp3" },
    { "Jazz Groove",     "http://east-mp3-128.streamthejazzgroove.com/stream" },
    { "Jazz London",     "http://radio.canstream.co.uk:8075/live.mp3" },
    { "SonicUniverse",   "https://ice6.somafm.com/sonicuniverse-128-mp3" },
    { "WBGO 88.3",       "https://ais-sa8.cdnstream1.com/3629_128.mp3" },
    { "KCSM Jazz91",     "http://ice7.securenetsystems.net/KCSM2" },
    { "SmoothJazz",      "https://smoothjazz.cdnstream1.com/2585_128.mp3" },
    { "101 SmoothJazz",  "http://jking.cdnstream1.com/b22139_128mp3" },
    { "Jazz Lounge",     "http://eu8.fastcast4u.com:5068/;" },
    /* Internacionalni */
    { "Radio Paradise",  "https://stream.radioparadise.com/mp3-192" },
    { "SomaFM Groove",   "https://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM Lush",     "https://ice1.somafm.com/lush-128-mp3" },
    { "SomaFM Drone",    "https://ice1.somafm.com/dronezone-128-mp3" },
};

#define STATION_COUNT ((int)(sizeof(stations) / sizeof(stations[0])))
#define MPD_DEFAULT_HOST "127.0.0.1"
#define MPD_DEFAULT_PORT 6600

#endif /* STATIONS_H */
