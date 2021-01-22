/*
 * Appcelerator Titanium Mobile
 * Copyright (c) 2011-Present by Appcelerator, Inc. All Rights Reserved.
 * Licensed under the terms of the Apache Public License
 * Please see the LICENSE included with this distribution for details.
 */
/* eslint-env mocha */
/* eslint no-unused-expressions: "off" */
'use strict';
const should = require('./utilities/assertions');

describe('Titanium.Media', () => {
	it('#createAudioPlayer()', () => should(Ti.Media.createAudioPlayer).be.a.Function());
});

describe.androidARM64Broken('Titanium.Media.AudioPlayer', function () {
	let audioPlayer;

	this.timeout(5000);

	beforeEach(() => {
		audioPlayer = Ti.Media.createAudioPlayer({ url: '/sample.mp3' });
	});

	afterEach(() => {
		// FIXME: calling release() on iOS is broken
		if (audioPlayer && Ti.App.Android) {
			audioPlayer.release();
		}
		audioPlayer = null;
	});

	it('apiName', function () {
		should(audioPlayer).have.a.readOnlyProperty('apiName').which.is.a.String();
		should(audioPlayer.apiName).be.eql('Ti.Media.AudioPlayer');
	});

	it('.url', function (finish) {
		should(audioPlayer.url).be.a.String();
		// getter/setters no longer exist!
		should(audioPlayer).have.accessors('url');
		// should(audioPlayer.url).eql(audioPlayer.getUrl());

		// Re-set URL to test TIMOB-26334, this should not crash
		try {
			audioPlayer.url = '/sample.mp3';
		} catch (e) {
			return finish(e);
		}
		finish();
	});

	it('#start, #stop', function (finish) {
		should(audioPlayer.start).be.a.Function();
		should(audioPlayer.stop).be.a.Function();

		audioPlayer.start();

		setTimeout(function () {
			try {
				audioPlayer.stop();
			} catch (e) {
				return finish(e);
			}
			finish();
		}, 1000);
	});

	it('#pause', function (finish) {
		should(audioPlayer.pause).be.a.Function();

		audioPlayer.start();

		setTimeout(function () {
			try {
				audioPlayer.pause();
			} catch (e) {
				return finish(e);
			}
			finish();
		}, 1000);
	});

	it.windowsBroken('#restart', function (finish) {
		should(audioPlayer.restart).be.a.Function();

		audioPlayer.start();

		setTimeout(function () {
			try {
				audioPlayer.restart();
				audioPlayer.stop();
			} catch (e) {
				return finish(e);
			}
			finish();
		}, 1000);
	});

	it.windowsMissing('.duration', function (finish) {
		audioPlayer.start();

		setTimeout(function () {
			try {
				should(audioPlayer.duration).be.a.Number();
				// give a tiny bit of fudge room here. iOS and Android differ by 5ms on this file
				should(audioPlayer.duration).be.within(45250, 45500); // 45 seconds. iOS gives us 45322, Android gives 45327
			} catch (e) {
				return finish(e);
			}
			finish();
		}, 1000);
	});

	it.ios('TIMOB-26533', function (finish) {
		//	Ti.Media.Audio player without url set is crashing while registering for event listener
		audioPlayer = null;
		audioPlayer = Ti.Media.createAudioPlayer();

		try {
			audioPlayer.addEventListener('progress', function () {});
		} catch (e) {
			return finish(e);
		}
		finish();
	});
});
