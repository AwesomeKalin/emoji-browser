// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.device.nfc;

import android.nfc.FormatException;
import android.nfc.NdefMessage;
import android.nfc.Tag;
import android.nfc.TagLostException;
import android.nfc.tech.Ndef;
import android.nfc.tech.NdefFormatable;
import android.nfc.tech.TagTechnology;

import org.chromium.device.mojom.NdefCompatibility;

import java.io.IOException;

/**
 * Utility class that provides I/O operations for NFC tags.
 */
public class NfcTagHandler {
    private final int mCompatibility;
    private final TagTechnology mTech;
    private final TagTechnologyHandler mTechHandler;
    private boolean mWasConnected;

    /**
     * Factory method that creates NfcTagHandler for a given NFC Tag.
     *
     * @param tag @see android.nfc.Tag
     * @return NfcTagHandler or null when unsupported Tag is provided.
     */
    public static NfcTagHandler create(Tag tag) {
        if (tag == null) return null;

        Ndef ndef = Ndef.get(tag);
        if (ndef != null) {
            int compatibility = NdefCompatibility.VENDOR;
            String type = ndef.getType();
            if (type.equals(Ndef.NFC_FORUM_TYPE_1) || type.equals(Ndef.NFC_FORUM_TYPE_2)
                    || type.equals(Ndef.NFC_FORUM_TYPE_3) || type.equals(Ndef.NFC_FORUM_TYPE_4)) {
                compatibility = NdefCompatibility.NFC_FORUM;
            }
            return new NfcTagHandler(compatibility, ndef, new NdefHandler(ndef));
        }

        NdefFormatable formattable = NdefFormatable.get(tag);
        if (formattable != null) {
            return new NfcTagHandler(
                    NdefCompatibility.VENDOR, formattable, new NdefFormattableHandler(formattable));
        }

        return null;
    }

    /**
     * NdefFormatable and Ndef interfaces have different signatures for operating with NFC tags.
     * This interface provides generic methods.
     */
    private interface TagTechnologyHandler {
        public void write(NdefMessage message)
                throws IOException, TagLostException, FormatException, IllegalStateException;
        public NdefMessage read()
                throws IOException, TagLostException, FormatException, IllegalStateException;
    }

    /**
     * Implementation of TagTechnologyHandler that uses Ndef tag technology.
     * @see android.nfc.tech.Ndef
     */
    private static class NdefHandler implements TagTechnologyHandler {
        private final Ndef mNdef;

        NdefHandler(Ndef ndef) {
            mNdef = ndef;
        }

        @Override
        public void write(NdefMessage message)
                throws IOException, TagLostException, FormatException, IllegalStateException {
            mNdef.writeNdefMessage(message);
        }

        @Override
        public NdefMessage read()
                throws IOException, TagLostException, FormatException, IllegalStateException {
            return mNdef.getNdefMessage();
        }
    }

    /**
     * Implementation of TagTechnologyHandler that uses NdefFormatable tag technology.
     * @see android.nfc.tech.NdefFormatable
     */
    private static class NdefFormattableHandler implements TagTechnologyHandler {
        private final NdefFormatable mNdefFormattable;

        NdefFormattableHandler(NdefFormatable ndefFormattable) {
            mNdefFormattable = ndefFormattable;
        }

        @Override
        public void write(NdefMessage message)
                throws IOException, TagLostException, FormatException, IllegalStateException {
            mNdefFormattable.format(message);
        }

        @Override
        public NdefMessage read() throws FormatException {
            return NfcTypeConverter.emptyNdefMessage();
        }
    }

    protected NfcTagHandler(int compatibility, TagTechnology tech, TagTechnologyHandler handler) {
        mCompatibility = compatibility;
        mTech = tech;
        mTechHandler = handler;
    }

    /**
     * Connects to NFC tag.
     */
    public void connect() throws IOException, TagLostException {
        if (!mTech.isConnected()) {
            mTech.connect();
            mWasConnected = true;
        }
    }

    /**
     * Checks if NFC tag is connected.
     */
    public boolean isConnected() {
        return mTech.isConnected();
    }

    /**
     * Closes connection.
     */
    public void close() throws IOException {
        mTech.close();
    }

    /**
     * Writes NdefMessage to NFC tag.
     */
    public void write(NdefMessage message)
            throws IOException, TagLostException, FormatException, IllegalStateException {
        mTechHandler.write(message);
    }

    public NdefMessage read()
            throws IOException, TagLostException, FormatException, IllegalStateException {
        return mTechHandler.read();
    }

    /**
     * If tag was previously connected and subsequent connection to the same tag fails, consider
     * tag to be out of range.
     */
    public boolean isTagOutOfRange() {
        try {
            connect();
        } catch (IOException e) {
            return mWasConnected;
        }
        return false;
    }

    /**
     * Returns NdefCompatibility.NFC_FORUM if the tag has a NFC standard type, otherwise returns
     * NdefCompatibility.VENDOR.
     */
    public int compatibility() {
        return mCompatibility;
    }
}
