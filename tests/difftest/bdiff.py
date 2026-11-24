import sys
rc = [l.split() for l in open(sys.argv[1])]
uc = [l.split() for l in open(sys.argv[2])]
labels = {r[0] for r in rc}
ucf = [u for u in uc if u[0] in labels]
for i, (r, u) in enumerate(zip(rc, ucf)):
    if r != u:
        print("first diff at record", i)
        for j in range(max(0, i - 3), i + 1):
            print(" rc", " ".join(rc[j]))
            print(" uc", " ".join(ucf[j]))
        break
else:
    print("no diff in", min(len(rc), len(ucf)), "records", len(rc), len(ucf))
