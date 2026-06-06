#include "buffer/clock_replacer.h"

#include "gtest/gtest.h"

TEST(CLOCKReplacerTest, BasicClockTest) {
  CLOCKReplacer clock_replacer(4);
  frame_id_t value;

  EXPECT_FALSE(clock_replacer.Victim(&value));

  clock_replacer.Unpin(0);
  clock_replacer.Unpin(1);
  clock_replacer.Unpin(2);
  clock_replacer.Unpin(1);
  EXPECT_EQ(3, clock_replacer.Size());

  EXPECT_TRUE(clock_replacer.Victim(&value));
  EXPECT_EQ(0, value);
  EXPECT_EQ(2, clock_replacer.Size());

  clock_replacer.Pin(1);
  EXPECT_EQ(1, clock_replacer.Size());

  clock_replacer.Unpin(0);
  clock_replacer.Unpin(3);
  EXPECT_EQ(3, clock_replacer.Size());

  EXPECT_TRUE(clock_replacer.Victim(&value));
  EXPECT_EQ(2, value);
  EXPECT_TRUE(clock_replacer.Victim(&value));
  EXPECT_EQ(3, value);
  EXPECT_TRUE(clock_replacer.Victim(&value));
  EXPECT_EQ(0, value);
  EXPECT_FALSE(clock_replacer.Victim(&value));
}

TEST(CLOCKReplacerTest, PinAndInvalidFrameTest) {
  CLOCKReplacer clock_replacer(2);
  frame_id_t value;

  clock_replacer.Unpin(-1);
  clock_replacer.Unpin(2);
  EXPECT_EQ(0, clock_replacer.Size());

  clock_replacer.Unpin(0);
  clock_replacer.Unpin(1);
  EXPECT_EQ(2, clock_replacer.Size());

  clock_replacer.Pin(0);
  clock_replacer.Pin(0);
  clock_replacer.Pin(2);
  EXPECT_EQ(1, clock_replacer.Size());

  EXPECT_TRUE(clock_replacer.Victim(&value));
  EXPECT_EQ(1, value);
  EXPECT_EQ(0, clock_replacer.Size());
}
