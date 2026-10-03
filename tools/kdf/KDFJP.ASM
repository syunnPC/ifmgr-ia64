;; SPDX-License-Identifier: GPL-2.0-or-later AND MIT
;; Its layout and macros follow MS-DOS 4.0's KDF files: Copyright (c)
;; Microsoft Corporation, MIT License (vendor/msdos4/LICENSE).
;; Keyboard Definition File for the Japanese 106/109-key keyboard (JP),
;; written for this project in MS-DOS 4's KDF language (KEYBMAC.INC's
;; macros, assembled by tools/mkkeyboard.py into KEYBOARD.SYS after the
;; vendored layouts). Scan codes are those of the keys' positions; the
;; JIS legends differ from US ones on the symbol keys:
;;   1-0 shifted: ! " # $ % & ' ( ) and nothing on 0
;;   -  =        ^  ~        @  `        [  {
;;   ;  +        :  *        ]  }        and the US ` key (Hankaku/Zenkaku)
;;   types nothing, being the input method's switch.
;; The two keys only Japanese keyboards have, Yen (7Dh: \ |) and Ro (73h:
;; \ _), lie past scan code 88 where DOS 4's INT 9 stops, so they reach the
;; state logic only where the firmware reports them; Henkan, Muhenkan and
;; Katakana/Hiragana type nothing without an input method. Code pages 932
;; (whose single-byte half shows 5Ch as the Yen sign) and 437 type the same
;; bytes, so both specific sections are empty.
				       ;;
	INCLUDE KEYBSHAR.INC	       ;;
	INCLUDE POSTEQU.INC	       ;;
	INCLUDE KEYBMAC.INC	       ;;
				       ;;
	PUBLIC JP_LANG_ENT	       ;;
				       ;;
CODE	SEGMENT PUBLIC 'CODE'          ;;
	ASSUME CS:CODE,DS:CODE	       ;;
				       ;;
STANDARD_TABLE	    EQU   TYPE_2_TAB+ASCII_ONLY
				       ;;
JP_LANG_ENT:			       ;; language entry for JAPAN
  DB   'JP'                            ;;
  DW   0			       ;; no keyboard ID
  DW   OFFSET JP_LOGIC,0	       ;; pointer to LANG kb table
  DB   1			       ;; number of IDs
  DB   2			       ;; number of code pages
  DW   932			       ;; code page
  DW   OFFSET JP_932_XLAT,0	       ;; table pointer
  DW   437			       ;; code page
  DW   OFFSET JP_437_XLAT,0	       ;; table pointer
				       ;;
;;***************************************
;; JP State Logic
;;***************************************
				       ;;
JP_LOGIC:			       ;;
   DW  LOGIC_END-$		       ;; length
   DW  0			       ;; special features
				       ;;
   OPTION EXIT_IF_FOUND 	       ;;
   IFF EITHER_ALT,NOT		       ;;
   ANDF EITHER_CTL,NOT		       ;;
      IFF EITHER_SHIFT		       ;;
	  XLATT NON_ALPHA_UPPER        ;;
      ELSEF			       ;;
	  XLATT NON_ALPHA_LOWER        ;;
      ENDIFF			       ;;
   ELSEF			       ;;
      IFF EITHER_CTL		       ;;
      ANDF EITHER_ALT,NOT	       ;;
	  XLATT CTRL_CASE	       ;;
      ENDIFF			       ;;
   ENDIFF			       ;;
   EXIT_STATE_LOGIC		       ;;
LOGIC_END:			       ;;
				       ;;
;;***************************************
;; JP Common Translate Section
;;***************************************
				       ;;
JP_COMMON_XLAT: 		       ;;
   DW	 COMMON_XLAT_END-$	       ;; length of section
   DW	 -1			       ;; code page
				       ;;
   DW	 COM_NA_LO_END-$	       ;; length of state section
   DB	 NON_ALPHA_LOWER	       ;; State ID
   DW	 ANY_KB 		       ;; Keyboard Type
   DB	 -1,-1			       ;; Buffer entry for error character
   DW	 COM_NA_LO_T1_END-$	       ;; Size of xlat table
   DB	 STANDARD_TABLE 	       ;; xlat options:
   DB	 10			       ;; number of entries
   DB	 13,'^'                        ;; = key
   DB	 26,'@'                        ;; [ key
   DB	 27,'['                        ;; ] key
   DB	 39,';'                        ;; ; key
   DB	 40,':'                        ;; ' key
   DB	 41,-1			       ;; ` key: Hankaku/Zenkaku
   DB	 43,']'                        ;; \ key
   DB	 115,'\'                       ;; Ro
   DB	 125,'\'                       ;; Yen
   DB	 12,'-'                        ;; - key
COM_NA_LO_T1_END:		       ;;
   DW	 0			       ;; Size of xlat table - null table
COM_NA_LO_END:			       ;;
				       ;;
   DW	 COM_NA_UP_END-$	       ;; length of state section
   DB	 NON_ALPHA_UPPER	       ;; State ID
   DW	 ANY_KB 		       ;; Keyboard Type
   DB	 -1,-1			       ;; Buffer entry for error character
   DW	 COM_NA_UP_T1_END-$	       ;; Size of xlat table
   DB	 STANDARD_TABLE 	       ;; xlat options:
   DB	 17			       ;; number of entries
   DB	 3,'"'                         ;; 2
   DB	 7,'&'                         ;; 6
   DB	 8,"'"                         ;; 7
   DB	 9,'('                         ;; 8
   DB	 10,')'                        ;; 9
   DB	 11,-1			       ;; 0: nothing
   DB	 12,'='                        ;; -
   DB	 13,'~'                        ;; =
   DB	 26,'`'                        ;; [
   DB	 27,'{'                        ;; ]
   DB	 39,'+'                        ;; ;
   DB	 40,'*'                        ;; '
   DB	 41,-1			       ;; `
   DB	 43,'}'                        ;; \
   DB	 115,'_'                       ;; Ro
   DB	 125,'|'                       ;; Yen
   DB	 2,'!'                         ;; 1
COM_NA_UP_T1_END:		       ;;
   DW	 0			       ;; Size of xlat table - null table
COM_NA_UP_END:			       ;;
				       ;;
   DW	 COM_CTRL_END-$ 	       ;; length of state section
   DB	 CTRL_CASE		       ;; State ID
   DW	 ANY_KB 		       ;; Keyboard Type
   DB	 -1,-1			       ;; Buffer entry for error character
   DW	 COM_CTRL_T1_END-$	       ;; Size of xlat table
   DB	 TYPE_2_TAB		       ;; xlat options:
   DB	 7			       ;; number of entries
   DB	 13,1EH,13		       ;; ctl ^
   DB	 26,0,3 		       ;; ctl @: NUL as the BIOS's ctl 2
   DB	 27,1BH,27		       ;; ctl [
   DB	 43,1DH,43		       ;; ctl ]
   DB	 41,-1,-1		       ;; ctl Hankaku/Zenkaku
   DB	 115,1CH,115		       ;; ctl Ro: \
   DB	 125,1CH,125		       ;; ctl Yen: \
COM_CTRL_T1_END:		       ;;
   DW	 0			       ;; Size of xlat table - null table
COM_CTRL_END:			       ;;
				       ;;
   DW	 0			       ;; Last State
COMMON_XLAT_END:		       ;;
				       ;;
;;***************************************
;; JP Specific Translate Sections
;;***************************************
				       ;;
JP_932_XLAT:			       ;;
   DW	 CP932_XLAT_END-$	       ;; length of section
   DW	 932			       ;; code page
   DW	 0			       ;; Last State
CP932_XLAT_END: 		       ;;
				       ;;
JP_437_XLAT:			       ;;
   DW	 CP437_XLAT_END-$	       ;; length of section
   DW	 437			       ;; code page
   DW	 0			       ;; Last State
CP437_XLAT_END: 		       ;;
				       ;;
CODE	ENDS			       ;;
	END			       ;;
