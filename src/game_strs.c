/* game_strs.c — the text table; see game_strs.h.
 *
 * Every column is verbatim from the shipped image of that language; a
 * pointer here is behaviour-identical to the literal it replaced.  The only
 * rewrite is GAME_STR_GAMEFLOW_LEVEL_DONE: the original patches three digits
 * in place (1000:33B6..33D1 divides by 0x64/0x0A and always writes three), so
 * the table carries a `%03u` where the shipped template has its `000`.
 * Generated and checked by `tools/extract_game_strs.py`.
 */
#include "game_strs.h"

const char *const game_strs[GAME_LANG_COUNT][GAME_STR_COUNT] = {
    [GAME_LANG_EN] = {
        /* attract */
        "FASTEST LEVEL TIMES!",
        "LEVEL         MINUTES,    SECONDS",
        "TOP TEN TRIBES",

        /* gameflow */
        "WELL DONE! YOU FINISHED LEVEL %03u",
        "YOUR TRIBE IS EXTINCT!!",

        /* level: the insert-disc page */
        "PUT THE DISK",
        "INTO DRIVE %c",
        "PRESS FIRE",
        "WHEN READY",

        /* map: the qualifying page */
        "WELL DONE!",
        "YOUR TRIBE HAS EVOLVED TO BECOME",
        "ONE OF THE TEN MOST ADVANCED IN",
        "THE ENTIRE WORLD!",
        "THE NAME OF YOUR TRIBE WILL NOW",
        "BE CARVED INTO THE ROCK THAT MARKS",
        "THE TEN MOST CIVILISED TRIBES!",
        "THE TOP TEN TRIBES",

        /* menu */
        "PLEASE SELECT YOUR OPTION\r"
        "\r"
        "CONTINUE THIS LEVEL\r"
        "RESTART THIS LEVEL\r"
        "RESTART THIS GAME\r"
        "SAVE GAME \r"
        "QUIT THIS GAME",
        "SAVE GAME NUMBER\r"
        "\r"
        " 01 05 09 13 17 \r"
        " 02 06 10 14 18 \r"
        " 03 07 11 15 19 \r"
        " 04 08 12 16 20 ",
        "YOU RAN OUT OF TIME!!\r"
        " \r"
        "RESTART THIS LEVEL\r"
        "RESTART THIS GAME\r"
        "QUIT THIS GAME",

        /* party intro */
        "COMING UP.... LEVEL 000",
        "YOU WILL NEED 0 HUMANS TO COMPLETE",
        "THIS LEVEL",
        "YOUR TRIBE NOW HAS 00 MEMBERS!",
        "THE CODE FOR THIS LEVEL IS.....",
        "YOUR SCORE IS 00000000000",
        "PRESS FIRE TO START LEVEL...",
        " MEMBERS!",
        "GAME OVER!",
        "YOUR TRIBE IS TOO SMALL TOO CONTINUE",
        "IN THE RACE FOR EVOLUTION!",
        "PRESS FIRE TO TRY AGAIN!",

        /* scene headlines */
        "HUMANS INVENT WEAPON! THE SPEAR!",
        "HUMANS RESCUE BABY DINOSAUR!",
        "REVOLUTION IN TRANSPORT! THE WHEEL!",
        "BRAVE HUMAN RESCUES THE QUEEN!",
        "SPECIAL REPORT ON NEW DISCOVERY! FIRE!",
        "UNIDENTIFIED FLYING HUMAN SPOTTED!",
        "HUMANS FAIL IN RACE FOR EVOLUTION!",
        "HUMANS WIN THE RACE FOR EVOLUTION!",
        "",
        "HUMANS RECOVER THE STOLEN IDOL!"
    },
    [GAME_LANG_FRE] = {
        /* attract */
        "MEILLIEURS TEMPS POUR LES NIVEAUX!",
        "NIVEAU        MINUTES     SECONDS",
        "LES DIX PREMIERES TRIBUS!",

        /* gameflow */
        "BIEN JOUE! VOUS AVEZ\rTERMINE LE NIVEAU %03u",
        "VOTRE TRIBU A DISPARU!!",

        /* level: the insert-disc page */
        "PUT THE DISK",
        "INTO DRIVE %c",
        "PRESS FIRE",
        "WHEN READY",

        /* map: the qualifying page (six lines, the seventh is the header) */
        "BIEN JOUE!",
        "VOTRE TRIBU EST DEVENU L'UNE DES",
        "DIX PREMIERES TRIBUS DU MONDE ENTIER!",
        "LE NOM DE VOTRE TRIBU VA ETRE GRAVE",
        "SUR LA PIERRE OU FIGURENT LES NOMS",
        "DES DIX TRIBUS LES PLUS CIVILISEES...",
        "LES DIX PREMIERES TRIBUS",
        "LES DIX PREMIERES TRIBUS",

        /* menu */
        "SELECTIONNEZ VOTRE OPTION\r"
        " \r"
        "PASSER AU NIVEAU SUIVANT\r"
        "RECOMMENCER CE NIVEAU\r"
        "RECOMMERCER CE JEU\r"
        "SAUVEGARDER PARTIE\r"
        "ABANDONNER LE JEU",
        "SAUVEGARDER NUMERO\r"
        "\r"
        " 01 05 09 13 17 \r"
        " 02 06 10 14 18 \r"
        " 03 07 11 15 19 \r"
        " 04 08 12 16 20 ",
        "TEMPS EPUISE!!\r"
        " \r"
        "RECOMMENCER CE NIVEAU\r"
        "RECOMMENCER CE JEU\r"
        "ABANDONNER LE JEU",

        /* party intro */
        "A VENIR.... NIVEAU 000",
        "VOUS AVEZ BESOIN DE 0 HUMANS POUR",
        "TERMINER CE NIVEAU",
        "VOTRE TRIBU A 00 MEMBRES!",
        "LE CODE DE CE NIVEAU EST.....",
        "VOTRE SCORE EST 00000000000",
        "APPUYER SUR FEU POUR COMMENCER LE NIVEAU",
        " MEMBRES!",
        "FIN DE PARTIE!",
        "VOTRE TRIBU EST TROP PETITE POUR",
        "CONTINUER SA COURSE VERS L'EVOLUTION!",
        "APPUYEZ SUR 'FEU' POUR RECOMMENCER!",

        /* scene headlines */
        "LES HUMANS INVENTENT L'ARME! LA LANCE",
        "LES HUMANS SAUVENT UN BEBE DINOSAURE!",
        "REVOLUTION DANS LES TRANSPORTS! LA ROUE!",
        "LE COURAGEUX HUMAN SAUVE LA REINE!",
        "NOUVELLE DECOUVERTE! LE FEU!",
        "HUMAN VOLANT NON-IDENTIFIE A ETE REPERE!",
        "ECHEC DANS LA COURSE A L'EVOLUTION!",
        "SUCCES DANS LA COURSE A L'EVOLUTION!",
        "",
        "LES HUMANS RETROUVENT L'IDOLE SACREE!"
    },
    [GAME_LANG_GER] = {
        /* attract */
        "DIE SCHNELLSTEN LEVELSTUERMER!",
        "LEVEL         MINUTEN     SEKUNDEN",
        "DIE ZEHN BESTEN STAEMME!",

        /* gameflow */
        "PRIMA DU HAST\rEBENE %03u ABSOLVIERT",
        "DEIN STAMM IST AUSGESTORBEN!",

        /* level: the insert-disc page */
        "PUT THE DISK",
        "INTO DRIVE %c",
        "PRESS FIRE",
        "WHEN READY",

        /* map: the qualifying page (six lines, the seventh is the header) */
        "GUT GEMACHT!",
        "DIEN STAMM HAT SICH ZU DER ZEHN",
        "BESTEN IS DER GESCHICHTE ENTWICKELT!",
        "DER NAME DEINES STAMMES WIRD NUN IN",
        "DEN STEIN DER ZEHN ZIVILISIERTESTEN",
        "VOELKER GRAVIERT!",
        "DIE ZEHN BESTEN STAEMME",
        "DIE ZEHN BESTEN STAEMME",

        /* menu */
        "BITTE DEINE OPTION WAHLEN\r"
        "\r"
        "AUF DIESER EBENE WEITERMACHEN\r"
        "DIESE EBENE NEU BEGINNEN\r"
        "DIESES SPIEL NEU BEGINNEN\r"
        "SPIEL SPEICHERN\r"
        "DIESES SPIEL VERLASSEN",
        "DIE SPIELNUMMER SPREICHERN\r"
        "\r"
        " 01 05 09 13 17 \r"
        " 02 06 10 14 18 \r"
        " 03 07 11 15 19 \r"
        " 04 08 12 16 20 ",
        "DIE ZEIT IST ABGELAUFEN!\r"
        " \r"
        "DIESE EBENE NEU BEGINNEN\r"
        "DIESES SPIEL NEU BEGINNEN\r"
        "DIESES SPIEL VERLASSEN",

        /* party intro */
        "ALS NAECHSTES..... LEVEL 000",
        "ES WERDEN 0 HUMANS BENOETIGT, UM DIESE",
        "RUNDE ZU SCHAFFEN",
        "DEIN STAMM HAT 00 MITGLIEDER!",
        "DER CODE FUER DIESE LEVEL IST.....",
        "DEIN SCORE IST 00000000000",
        "DRUECKE FIRE FUER DEN START...",
        " MITGLIEDER!",
        "GAME OVER!",
        "DEINE GRUPPE IST ZU KLEIN, UM WIETERHIN",
        "AM FORTGANG DER EVOLUTION TEILZUNEHMEN",
        "DRUECKE FEUER FUER EINEN NEUEN VERSUCH",

        /* scene headlines */
        "HUMAN ENTDECKT WAFFE! DER SPEER!",
        "HUMAN RETTET DINO BABY!",
        "DAS RAD... REVOLUTIONAERER FORTSCHRITT!",
        "KOENIGIN VON TAPFEREM HUMAN GERETTET!",
        "NEUE ENTDECKUNG.. DAS FEUER!",
        "FLIEGENDER HUMAN ENTDECKT!",
        "DAS RENNEN DER EVOLUTION VERLOREN!",
        "DAS RENNEN DER EVOLUTION GEWONNEN!",
        "",
        "HUMANS SICHERTEN HEILIGES GOETZENBILD!"
    },
    [GAME_LANG_ITA] = {
        /* attract */
        "I TEMPI PIU' RAPIDI!",
        "LIVELLO         MINUTI      SECONDI",
        "LE DIECI TRIBU' MIGLIORI!",

        /* gameflow */
        "COMPLIMENTI HAI\rFINITO IL LIVELLO %03u",
        "LA TUA TRIBU' E' ESTINTA!!",

        /* level: the insert-disc page */
        "PUT THE DISK",
        "INTO DRIVE %c",
        "PRESS FIRE",
        "WHEN READY",

        /* map: the qualifying page */
        "BEN FATTO!",
        "LA TUA TRIBU' SI E' EVOLUTA ED E'",
        "DIVENTATA UNA DELLE DIECI PIU'!",
        "PROGREDITE NEL MONDO!",
        "IL NOME DELLA TRIBU' VERRA' INCISO",
        "NELLA ROCCIA CHE TIENE LE DIECI",
        "TRIBU' PIU' CIVILIZZATE!",
        "LE DIECI TRIBU' MIGLIORI",

        /* menu */
        "SELEZIONALA TUE OPZIONE\r"
        "\r"
        "CONTINUA QUESTO LIVELLO\r"
        "INIZIA DI NUOVO QUESTO LIVELLO\r"
        "INIZIA DI NUOVO QUESTO GIOCO\r"
        "SALVA IL GIOCO\r"
        "ESCI DA QUESTO GIOCO",
        "SALVA NUMERO\r"
        "\r"
        " 01 05 09 13 17 \r"
        " 02 06 10 14 18 \r"
        " 03 07 11 15 19 \r"
        " 04 08 12 16 20 ",
        "STAI ESAURENDO IL TEMPO\r"
        "         A DISPOSIZIONE\r"
        "INIZIA DI NUOVO QUESTO LIVELLO\r"
        "INIZIA DI NUOVO QUESTO GIOCO\r"
        "ESCI DA QUESTO GIOCO",

        /* party intro */
        "STA ARRIVANDO.... LIVELLO 000",
        "HAI BISOGNO DI 0 HUMANS PER COMPLETARE",
        "QUESTO LIVELLO!",
        "LA TUA TRIBU' HA 00 MEMBRI!",
        "IL CODICE PER IL LIVELLO E' .....",
        "IL TUO PUNTEGGIO E' 00000000000",
        "PREMI FUOCO PER INIZIARE IL LIVELLO!",
        " MEMBRI!",
        "GAME OVER!",
        "LA TUA TRIBU 'E' TROPPO PICCOLA PER",
        "CONTINUARE LA CORSA PER L'EVOLUZIONE!",
        "PREMI FUOCO PER RIPROVARE!",

        /* scene headlines */
        "HUMANS INVENTANO L'ARMA! LA LANCIA!",
        "HUMANS SALVANO BABY DINOSAURO!",
        "RIVOLUZIONE NEI TRASPORTI! LA RUOTA!",
        "PRODI HUMANS SALVANO LA REGINA!",
        "INCREDIBILE SCOPERTA! IL FUOCO!",
        "AVVISTATO UOMO VOLANTE NON IDENTIFICATO!",
        "FALLITA LA CORSA PER L'EVOLUZIONE!",
        "HUMANS VINCONO LA CORSA PER L'EVOLUZIONE!",
        "",
        "HUMANS RECUPERANO L'IDOLO SACRO!"
    },
    [GAME_LANG_SPA] = {
        /* attract */
        "TIEMPOS MAS RAPIDOS DEL NIVEL",
        "NIVEL         MINUTES,    SECONDS",
        "LAS DIEZ MEJORES TRIBUS!",

        /* gameflow */
        "MUY BIEN HAS\rTERMINADO EL NIVAL %03u",
        "TU TRIBU SE HA EXTINGUIDO",

        /* level: the insert-disc page */
        "PUT THE DISK",
        "INTO DRIVE %c",
        "PRESS FIRE",
        "WHEN READY",

        /* map: the qualifying page */
        "BIEN HECHO!",
        "TU TRIBU HA EVOLUCIONADO HASTA",
        "CONVERTIRSE EN UNA DE LAS DIEZ",
        "MAS AVANZADAS DEL MUNDO ENTERO!",
        "AHORA EL NOMBRE DE TU TRIBU SERA",
        "GRABADO EN LA ROCA QUE MUESTRA",
        "LAS DIEZ TRIBUS MAS CIVILIZADAS!",
        "LAS DIEZ MEJORES TRIBUS",

        /* menu */
        "ELIGE UNA OPCION\r"
        "\r"
        "CONTINUAR EN ESTE NIVEL\r"
        "RECOMENZAR ESTE NIVEL\r"
        "REINICIAR LA PARTIDA\r"
        "SALVAR ESTE JUEGO\r"
        "ABANDONAR ESTA PARTIDA",
        "SALVAR NUMERO\r"
        "\r"
        " 01 05 09 13 17 \r"
        " 02 06 10 14 18 \r"
        " 03 07 11 15 19 \r"
        " 04 08 12 16 20 ",
        "SE TE ACABO EL TIEMPO\r"
        " \r"
        "REEMPEZAR EN ESTE NIVAL\r"
        "REEMPEZAR ESTE JUEGO\r"
        "ABANDONAR ESTE JUEGO",

        /* party intro */
        "EMPEZAMOS.... NIVEL 000",
        "NECESITARAS 0 HUMANS PARA COMPLETAR",
        "ESTE NIVEL!",
        "AHORA TU TRIBU TIENE 00 MIEMBROS!",
        "EL CODIGO DE ESTE NIVEL ES.....",
        "TU PUNTUACION ES 00000000000",
        "PULSA FUEGO PARA INICIAR NIVEL!",
        " MIEMBROS!",
        "FIN!",
        "TU TRIBU ES DEMASIADO REDUCIDA PARA",
        "CONTINUAR EN LA CARRERA EVOLUTIVA!",
        "PULSA FUEGO PARA INTENTARLO DE NUEVO!",

        /* scene headlines */
        "LOS HANS INVENTADO LA LANZA!",
        "LOS HUMANS RESCATARON UN DINOSAURIO!",
        "REVOLUCION DEL TRANSPORTE, LA RUEDA!",
        "EL BRAVO HUMAN RESCATO A LA REINA!",
        "NUEVO DESCUBRIMIENTO! EL FUEGO!",
        "HUMAN VOLADOR NO IDENTIFICADO!",
        "PIERDEN LA CARRERA DE LA EVOLUCION!",
        "GANAN LA CARRERA DE LA EVOLUCION!",
        "",
        "LOS HUMANS RECUPERAN EL IDOLO SAGRADO!"
    }
};

/* One row per language, one column per string, in the same order as
 * game_strs: the party templates' digit fields sit at fixed byte offsets in
 * the original's string block, and the other strings have no in-place field.
 * Every row is the index of that language's first digit in its own template
 * (tools/extract_game_strs.py derives it from the text, not from English). */
const uint8_t game_strs_num_offset[GAME_LANG_COUNT][GAME_STR_COUNT] = {
    [GAME_LANG_EN] = {
        /* attract */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* gameflow (printf formats) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* level (INTO DRIVE is a printf format) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE,
        /* map */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* menu */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* party intro: the in-place digit fields (202F:0x105F..) */
        0x14u,             /* COMING UP.... LEVEL nnn                     */
        0x0Eu,             /* YOU WILL NEED n HUMANS TO COMPLETE          */
        GAME_STR_NUM_NONE, /* THIS LEVEL                                  */
        0x13u,             /* YOUR TRIBE NOW HAS nn MEMBERS!              */
        GAME_STR_NUM_NONE, /* THE CODE FOR THIS LEVEL IS.....             */
        0x0Eu,             /* YOUR SCORE IS nnnn                          */
        GAME_STR_NUM_NONE, /* PRESS FIRE TO START LEVEL...                */
        GAME_STR_NUM_NONE, /* " MEMBERS!" tail                            */
        GAME_STR_NUM_NONE, /* GAME OVER!                                  */
        GAME_STR_NUM_NONE, /* YOUR TRIBE IS TOO SMALL TOO CONTINUE        */
        GAME_STR_NUM_NONE, /* IN THE RACE FOR EVOLUTION!                  */
        GAME_STR_NUM_NONE, /* PRESS FIRE TO TRY AGAIN!                    */
        /* scene headlines */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE
    },
    [GAME_LANG_FRE] = {
        /* attract */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* gameflow (printf formats) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* level (INTO DRIVE is a printf format) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE,
        /* map */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* menu */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* party intro */
        0x13u,             /* A VENIR.... NIVEAU nnn                      */
        0x14u,             /* VOUS AVEZ BESOIN DE n HUMANS POUR           */
        GAME_STR_NUM_NONE, /* TERMINER CE NIVEAU                          */
        0x0Eu,             /* VOTRE TRIBU A nn MEMBRES!                   */
        GAME_STR_NUM_NONE, /* LE CODE DE CE NIVEAU EST.....               */
        0x10u,             /* VOTRE SCORE EST nnnn                        */
        GAME_STR_NUM_NONE, /* APPUYER SUR FEU POUR COMMENCER LE NIVEAU    */
        GAME_STR_NUM_NONE, /* " MEMBRES!" tail                            */
        GAME_STR_NUM_NONE, /* FIN DE PARTIE!                              */
        GAME_STR_NUM_NONE, /* VOTRE TRIBU EST TROP PETITE POUR            */
        GAME_STR_NUM_NONE, /* CONTINUER SA COURSE VERS L'EVOLUTION!       */
        GAME_STR_NUM_NONE, /* APPUYEZ SUR 'FEU' POUR RECOMMENCER!         */
        /* scene headlines */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE
    },
    [GAME_LANG_GER] = {
        /* attract */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* gameflow (printf formats) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* level (INTO DRIVE is a printf format) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE,
        /* map */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* menu */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* party intro */
        0x19u,             /* ALS NAECHSTES..... LEVEL nnn                */
        0x0Au,             /* ES WERDEN n HUMANS BENOETIGT, UM DIESE      */
        GAME_STR_NUM_NONE, /* RUNDE ZU SCHAFFEN                           */
        0x0Fu,             /* DEIN STAMM HAT nn MITGLIEDER!               */
        GAME_STR_NUM_NONE, /* DER CODE FUER DIESE LEVEL IST.....          */
        0x0Fu,             /* DEIN SCORE IST nnnn                         */
        GAME_STR_NUM_NONE, /* DRUECKE FIRE FUER DEN START...              */
        GAME_STR_NUM_NONE, /* " MITGLIEDER!" tail                         */
        GAME_STR_NUM_NONE, /* GAME OVER!                                  */
        GAME_STR_NUM_NONE, /* DEINE GRUPPE IST ZU KLEIN, UM WIETERHIN     */
        GAME_STR_NUM_NONE, /* AM FORTGANG DER EVOLUTION TEILZUNEHMEN      */
        GAME_STR_NUM_NONE, /* DRUECKE FEUER FUER EINEN NEUEN VERSUCH      */
        /* scene headlines */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE
    },
    [GAME_LANG_ITA] = {
        /* attract */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* gameflow (printf formats) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* level (INTO DRIVE is a printf format) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE,
        /* map */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* menu */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* party intro */
        0x1Au,             /* STA ARRIVANDO.... LIVELLO nnn               */
        0x0Fu,             /* HAI BISOGNO DI n HUMANS PER COMPLETARE      */
        GAME_STR_NUM_NONE, /* QUESTO LIVELLO!                             */
        0x11u,             /* LA TUA TRIBU' HA nn MEMBRI!                 */
        GAME_STR_NUM_NONE, /* IL CODICE PER IL LIVELLO E' .....           */
        0x14u,             /* IL TUO PUNTEGGIO E' nnnn                    */
        GAME_STR_NUM_NONE, /* PREMI FUOCO PER INIZIARE IL LIVELLO!        */
        GAME_STR_NUM_NONE, /* " MEMBRI!" tail                             */
        GAME_STR_NUM_NONE, /* GAME OVER!                                  */
        GAME_STR_NUM_NONE, /* LA TUA TRIBU 'E' TROPPO PICCOLA PER         */
        GAME_STR_NUM_NONE, /* CONTINUARE LA CORSA PER L'EVOLUZIONE!       */
        GAME_STR_NUM_NONE, /* PREMI FUOCO PER RIPROVARE!                  */
        /* scene headlines */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE
    },
    [GAME_LANG_SPA] = {
        /* attract */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* gameflow (printf formats) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* level (INTO DRIVE is a printf format) */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE,
        /* map */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* menu */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        /* party intro */
        0x14u,             /* EMPEZAMOS.... NIVEL nnn                     */
        0x0Cu,             /* NECESITARAS n HUMANS PARA COMPLETAR         */
        GAME_STR_NUM_NONE, /* ESTE NIVEL!                                 */
        0x15u,             /* AHORA TU TRIBU TIENE nn MIEMBROS!           */
        GAME_STR_NUM_NONE, /* EL CODIGO DE ESTE NIVEL ES.....             */
        0x11u,             /* TU PUNTUACION ES nnnn                       */
        GAME_STR_NUM_NONE, /* PULSA FUEGO PARA INICIAR NIVEL!             */
        GAME_STR_NUM_NONE, /* " MIEMBROS!" tail                           */
        GAME_STR_NUM_NONE, /* FIN!                                        */
        GAME_STR_NUM_NONE, /* TU TRIBU ES DEMASIADO REDUCIDA PARA         */
        GAME_STR_NUM_NONE, /* CONTINUAR EN LA CARRERA EVOLUTIVA!          */
        GAME_STR_NUM_NONE, /* PULSA FUEGO PARA INTENTARLO DE NUEVO!       */
        /* scene headlines */
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE, GAME_STR_NUM_NONE, GAME_STR_NUM_NONE,
        GAME_STR_NUM_NONE
    }
};
