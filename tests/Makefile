CFLAGS?=-Wall -std=c90
TEST_ROUNDTRIP=./test_roundtrip

all: roundtrip

roundtrip: test_roundtrip.c ../fastlz.c
	$(CC) -o $(TEST_ROUNDTRIP)  $(CFLAGS) -I.. test_roundtrip.c ../fastlz.c
	$(TEST_ROUNDTRIP)

clean :
	$(RM) $(TEST_ROUNDTRIP) *.o
