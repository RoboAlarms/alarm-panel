proposal({
  bl: 'BL-048', title: 'Nearby keypad enrollment with automatic Wi-Fi', status: 'draft', role: 'installer',
  why: ['A nearby keypad receives Wi-Fi credentials only after both pairing confirmations. Select Wi-Fi remains available on the keypad when out of range.'],
  look: [['partitions', 'Add device > Keypad: nearby enrollment instructions']], replaces: ['partitions'],
  notes: ['Same enrollment popup on normal and headless masters. Language, display and audio remain local.',
    'Touchscreen flow: http://localhost:8544/#bl-048-radio-start. Transfer and retry screens are draft mockups, not implemented firmware.'],
  apply() {
    const original = keypadEnrollment;
    keypadEnrollment = function(resume) {
      original(resume);
      if (!resume) {
        const steps = document.querySelector('.enroll-steps');
        if (steps) steps.innerHTML = '<li>Choose Keypad in its setup.</li><li>Tap Find panel. If out of range, tap Select Wi-Fi.</li>';
      }
    };
  }
});
