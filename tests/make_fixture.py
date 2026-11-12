#!/usr/bin/env python3

TOTAL = 6
DATE = "2030/01/15"
CAL = [11, 33, 88, 100, 113, 188, 227]
XR = [11, 35, 85, 134, 196, 225]


def at(*pairs):
    line = ""
    for col, text in pairs:
        line = line.ljust(col) + text if len(line) < col else line + "  " + text
    return line.rstrip()


def bold(text):
    return "      ".join(text.split(" "))


def header(frn, page, banner=None):
    out = ["\f" + at((114, bold("FRT Report")), (205, f"Valid as of: {DATE}")),
           at((94, bold(f"Firearm Reference Number (FRN): {frn}")), (205, f"Page: {page} / {TOTAL}"))]
    if banner:
        out += [at((113, f"Make:      {banner[0]}")),
                at((7, "Summary"), (98, f"Model:       {bold(banner[1])}")), ""]
    return out


FOOTER = ["", "", at((50, "Firearms Reference Table, National Specialized Services, Royal Canadian "
                          "Mounted Police. All rights reserved, RCMP, 1998-2030."))]


def summary(fields):
    return [at((12, k + ":"), (57, v)) if v else at((12, k + ":")) for k, v in fields] + ["", ""]


def cal_header():
    return [at((7, bold("Calibre, Shots and Barrel Length"))), "",
            at((113, "Legal Classification")), at((100, "Barrel")),
            at(*zip(CAL, ["FRN", "Calibre", "Shots", "(mm)", "Legal Authority", "Level", "Barrel Type Code"]))]


def cal_row(frn, cal, shots, mm, cls, auth, level, level2=None, btc=None):
    first = [(CAL[0], frn), (CAL[1], cal), (CAL[2], shots), (CAL[3], mm), (CAL[4], cls), (CAL[5], level)]
    if btc:
        first.append((CAL[6], btc))
    out = [at(*first)]
    if level2:
        out.append(at((CAL[5], level2)))
    out.append(at((CAL[4], auth)))
    return out


def xref_header():
    return at(*zip(XR, ["FRN", "Make", "Model", "Manufacturer", "Type", "Action"]))


def xref(*cells):
    return at(*[(c, t) for c, t in zip(XR, cells) if t])


lines = []
lines += ["\fLegal disclaimer", "This document is not a legal instrument. (synthetic test fixture)", "", ""]
lines += ["\fTable of contents placeholder", ""]

F = "900001"
lines += header(F, 3, ("Acme Test Arms", "T-1 Carbine"))
lines += summary([("Make", "Acme Test Arms"), ("Model", "T-1 Carbine"), ("Manufacturer", "Acme Test Arms Ltd"),
                  ("Level", "Manufacturer Specifications and Commercial Customization"), ("Type", "Rifle"),
                  ("Action", "Semi-Automatic"), ("Country of Manufacturer", "CANADA"),
                  ("Serial Numbering", "See Note"), ("Legal Classification", "Restricted")])
lines += cal_header()
lines += cal_row(f"{F} - 1", "5.56MM NATO", "5", "470", "Restricted", 'CC 84(1) "restricted firearm" para. (c)',
                 "Manufacturer Specifications and", "Commercial Customization")
lines += cal_row(f"{F}-2", "5.56MM NATO", "30", "470", "Prohibited", "PFR, Part 1, para. 87(a)",
                 "Non-Commercial Customization", btc="Conversion Unit")
lines += cal_row(f"{F} - 3", '6.5X55 SWEDISH MAUSER/12 GA X 2 3/4"', "2", "635", "Non-Restricted",
                 'CC 2 "firearm"', "Manufacturer Specifications and", "Commercial Customization")
lines += ["", "", at((7, "Notes")), ""]
lines += [at((11, "Make"), (55, '- "ACME" is marked on the left side of the receiver/frame.')),
          at((55, "- Acme Test Arms Ltd is a fictional company used for testing.")), "", ""]
lines += [at((11, "Model")),
          at((55, "- features include: a very long description that wraps onto a second line because")),
          at((55, "it is longer than the column is wide.")),
          at((55, "- second model note.")), "", ""]
lines += FOOTER
lines += header(F, 4)
lines += ["", at((11, "Also Known As/Product"), (55, "- this label wraps onto a later line in the real report and")),
          at((55, "the parser must join it back together.")),
          at((11, "Code")), "", "",
          at((11, "Serial Number"), (55, "- serial number is marked on the receiver/frame.")), "", ""]
lines += [at((7, bold("Cross-References"))), xref_header(),
          xref("900002", "Acme Test Arms", "T-1 Pistol", "Acme Test Arms Ltd", "Handgun", "Semi-Automatic")]
lines += FOOTER
lines += header(F, 5)
lines += [xref("900003", "Bogus Mfg", "BX", "", "Shotgun", "Multi-Barrel"), "", ""]
lines += [at((7, bold("Also Known As/Product Code"))), at((11, "T1")), "", at((11, "T-1C")), "",
          "JP-T1", "", ""]
lines += [at((7, bold("Year Dates"))), at((11, "2001")), "", at((11, "2002")), "",
          at((11, "No/Aucune Date")), "", ""]
lines += [at((7, "Importer")), at((11, "Example Importing Co.")), "", at((11, "Another Importer Inc.")), ""]
lines += FOOTER

F = "900002"
lines += header(F, 6, ("Acme Test Arms", "T-1 Pistol"))
lines += summary([("Make", "Acme Test Arms"), ("Model", "T-1 Pistol"), ("Manufacturer", "Acme Test Arms Ltd"),
                  ("Level", "Manufacturer Specifications and Commercial Customization"), ("Type", "Handgun"),
                  ("Action", "Semi-Automatic"), ("Country of Manufacturer", "CANADA"),
                  ("Serial Numbering", ""), ("Legal Classification", "Prohibited")])
lines += [at((7, bold("Calibre, Shots and Barrel Length"))), at((51, "No Data")), "",
          at((7, "Notes")), "", at((11, "Canadian Law Comments"), (55, "- fictional record for testing.")), "", ""]
lines += [at((7, bold("Cross-References"))), xref_header(),
          xref("900001", "Acme Test Arms", "T-1 Carbine", "Acme Test Arms Ltd", "Rifle", "Semi-Automatic"), "", ""]
lines += [at((7, bold("Also Known As/Product Code")), (60, "No Data")), "",
          at((7, bold("Year Dates")), (60, "No Data")), "", at((7, "Importer"), (60, "No Data"))]
lines += FOOTER

print("\n".join(lines))
