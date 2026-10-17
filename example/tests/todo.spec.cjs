const { test, expect } = require('@playwright/test');
const path = require('node:path');
const fs = require('node:fs');

const screenshotDir = path.resolve(__dirname, '../screenshots');
if (!fs.existsSync(screenshotDir)) {
    fs.mkdirSync(screenshotDir, { recursive: true });
}

test('Todo App Interaction and Screenshots', async ({ page }) => {
    const filePath = 'file://' + path.resolve(__dirname, '../index.html');
    await page.goto(filePath);

    // Add tasks
    const tasks = ['Learn Gemini CLI', 'Build Todo App', 'Generate Screenshots'];
    for (const task of tasks) {
        await page.fill('#todo-input', task);
        await page.click('#add-btn');
    }

    // Complete one task
    await page.click('.todo-item:nth-child(2) .checkbox');

    // Screenshot of "All" view
    await page.screenshot({ path: path.join(screenshotDir, 'todo-all.png') });

    // Filter by "Active" and screenshot
    await page.click('[data-filter="active"]');
    await page.screenshot({ path: path.join(screenshotDir, 'todo-active.png') });

    // Filter by "Completed" and screenshot
    await page.click('[data-filter="completed"]');
    await page.screenshot({ path: path.join(screenshotDir, 'todo-completed.png') });

    console.log('Screenshots generated in example/screenshots/');
});
