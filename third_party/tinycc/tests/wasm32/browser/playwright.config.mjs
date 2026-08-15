import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
  testDir: '.',
  timeout: 30_000,
  use: {
    ...devices['Desktop Chrome'],
    browserName: 'chromium',
  },
  reporter: [['list']],
});
