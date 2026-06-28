// Saitama — Lang.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
#include "Lang.h"
#include "Config.h"

namespace ops {
namespace lang {

const char* const kLangNames[LANG_COUNT] = {
    "English", "Italiano", "Francais", "Deutsch", "Espanol"
};

// Translations are ASCII-safe (no accented characters) to guarantee
// rendering with any LVGL font configuration.
static const char* const s_strings[LANG_COUNT][TR_COUNT] = {
    // ── EN ──────────────────────────────────────────────────────────────
    {
        "Device Name",          // TR_DEVICE_NAME
        "Share My Contact",     // TR_SHARE_CONTACT
        "Generate Identity",    // TR_GEN_IDENTITY
        "Channels",             // TR_CHANNELS
        "Radio",                // TR_RADIO
        "Power",                // TR_POWER
        "LoRa Duty Cycle",      // TR_LORA_DUTY
        "CPU Governor",         // TR_CPU_GOV
        "Brightness",           // TR_BRIGHTNESS
        "Theme",                // TR_THEME
        "Font",                 // TR_FONT
        "Bluetooth",            // TR_BLUETOOTH
        "Speaker",              // TR_SPEAKER
        "GPS",                  // TR_GPS
        "Keyboard Light",       // TR_KB_LIGHT
        "Keyboard Layout",      // TR_KB_LAYOUT
        "Date / Time",          // TR_DATE_TIME
        "Timezone",             // TR_TIMEZONE
        "Firmware Update",      // TR_FW_UPDATE
        "Auto Add Contacts",    // TR_AUTO_ADD
        "Screen Timeout",       // TR_SCR_TIMEOUT
        "Screen Off",           // TR_SCR_OFF
        "Volume",               // TR_VOLUME
        "Notifications",        // TR_NOTIFICATIONS
        "Notification Sound",   // TR_NOTIFY_SOUND
        "Save Messages",        // TR_SAVE_MSGS
        "Show Hops",            // TR_SHOW_HOPS
        "Show RSSI",            // TR_SHOW_RSSI
        "Location Sharing",     // TR_LOCATION
        "Backup & Restore",     // TR_BACKUP
        "Return to Launcher",   // TR_RETURN
        "Language",             // TR_LANGUAGE
        "On",                   // TR_ON
        "Off",                  // TR_OFF
    },
    // ── IT ──────────────────────────────────────────────────────────────
    {
        "Nome Dispositivo",
        "Condividi Contatto",
        "Genera Identita",
        "Canali",
        "Radio",
        "Potenza",
        "Ciclo LoRa",
        "Governatore CPU",
        "Luminosita",
        "Tema",
        "Carattere",
        "Bluetooth",
        "Altoparlante",
        "GPS",
        "Luce Tastiera",
        "Layout Tastiera",
        "Data / Ora",
        "Fuso Orario",
        "Aggiorna Firmware",
        "Aggiungi Contatti Auto",
        "Timeout Schermo",
        "Schermo Off",
        "Volume",
        "Notifiche",
        "Suono Notifica",
        "Salva Messaggi",
        "Mostra Salti",
        "Mostra RSSI",
        "Condividi Posizione",
        "Backup & Ripristino",
        "Torna al Launcher",
        "Lingua",
        "Attivo",
        "Inattivo",
    },
    // ── FR ──────────────────────────────────────────────────────────────
    {
        "Nom Appareil",
        "Partager Contact",
        "Generer Identite",
        "Canaux",
        "Radio",
        "Puissance",
        "Cycle LoRa",
        "Gouverneur CPU",
        "Luminosite",
        "Theme",
        "Police",
        "Bluetooth",
        "Haut-parleur",
        "GPS",
        "Eclairage Clavier",
        "Disposition Clavier",
        "Date / Heure",
        "Fuseau Horaire",
        "Mise a jour FW",
        "Ajout Auto Contacts",
        "Delai Ecran",
        "Extinction Ecran",
        "Volume",
        "Notifications",
        "Son Notification",
        "Sauvegarder Msgs",
        "Afficher Sauts",
        "Afficher RSSI",
        "Partage Localisation",
        "Sauvegarde & Restaur.",
        "Retour Launcher",
        "Langue",
        "Actif",
        "Inactif",
    },
    // ── DE ──────────────────────────────────────────────────────────────
    {
        "Geraetename",
        "Kontakt teilen",
        "Identitaet erstellen",
        "Kanaele",
        "Radio",
        "Leistung",
        "LoRa Zyklus",
        "CPU-Regler",
        "Helligkeit",
        "Thema",
        "Schriftart",
        "Bluetooth",
        "Lautsprecher",
        "GPS",
        "Tastaturlicht",
        "Tastaturlayout",
        "Datum / Zeit",
        "Zeitzone",
        "Firmware-Update",
        "Kontakte auto add.",
        "Bildschirmtimeout",
        "Bildschirm aus",
        "Lautstaerke",
        "Benachrichtigung",
        "Benachr.-Ton",
        "Nachr. speichern",
        "Hops anzeigen",
        "RSSI anzeigen",
        "Standort teilen",
        "Sicherung & Wiederherst.",
        "Zurueck Launcher",
        "Sprache",
        "Ein",
        "Aus",
    },
    // ── ES ──────────────────────────────────────────────────────────────
    {
        "Nombre Dispositivo",
        "Compartir Contacto",
        "Generar Identidad",
        "Canales",
        "Radio",
        "Potencia",
        "Ciclo LoRa",
        "Gobernador CPU",
        "Brillo",
        "Tema",
        "Fuente",
        "Bluetooth",
        "Altavoz",
        "GPS",
        "Luz Teclado",
        "Disposicion Teclado",
        "Fecha / Hora",
        "Zona Horaria",
        "Actualizar Firmware",
        "Agregar Contactos Auto",
        "Tiempo Pantalla",
        "Pantalla Apagada",
        "Volumen",
        "Notificaciones",
        "Sonido Notificacion",
        "Guardar Mensajes",
        "Mostrar Saltos",
        "Mostrar RSSI",
        "Compartir Ubicacion",
        "Copia & Restauracion",
        "Volver Launcher",
        "Idioma",
        "Activo",
        "Inactivo",
    },
};

const char* tr(TrKey key) {
    uint8_t lang = ops::config::get().uiLanguage;
    if (lang >= LANG_COUNT) lang = LANG_EN;
    return s_strings[lang][key];
}

}  // namespace lang
}  // namespace ops
