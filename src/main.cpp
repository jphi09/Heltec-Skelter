#include "main.h"

// Create tracker instance
HTITTracker tracker;

void setup() {
    tracker.begin();
}

void loop() {
    tracker.update();
}
