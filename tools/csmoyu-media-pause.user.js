// ==UserScript==
// @name         CSMoyu - 切出网页时暂停视频
// @namespace    https://github.com/csmoyu
// @version      1.0.0
// @description  从哔哩哔哩或抖音切到 CS2 等其他应用时自动暂停视频
// @match        https://*.bilibili.com/*
// @match        https://*.douyin.com/*
// @run-at       document-start
// @grant        none
// ==/UserScript==

(() => {
    'use strict';

    let lastPauseAt = 0;

    function pauseAllMedia() {
        // blur 和 visibilitychange 往往会连续触发，合并为一次操作。
        const now = Date.now();
        if (now - lastPauseAt < 150) return;
        lastPauseAt = now;

        document.querySelectorAll('video, audio').forEach((media) => {
            if (!media.paused && !media.ended) {
                media.pause();
            }
        });
    }

    // Alt+Tab、Win+D、点击其他程序窗口时触发。
    window.addEventListener('blur', pauseAllMedia, true);

    // 切换标签页或最小化浏览器时触发，作为 blur 的补充。
    document.addEventListener('visibilitychange', () => {
        if (document.hidden) pauseAllMedia();
    }, true);

    // 页面正在退出时也尽力暂停。
    window.addEventListener('pagehide', pauseAllMedia, true);
})();
