import sys
sys.stdout.write("hi\n")
print(len(sys.argv))
sys.stderr.write("")
print("before exit")
sys.exit(0)
print("never")
