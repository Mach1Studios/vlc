/*****************************************************************************
 * Copyright (C) 2020 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * ( at your option ) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
import QtQuick 2.11
import QtQuick.Controls 2.4

import "qrc:///style/"

ToolTip {
    id: pointingTooltip

    margins: 0
    padding: VLCStyle.margin_xxsmall

    x: _x
    y: pos.y - (implicitHeight + arrowArea.implicitHeight + VLCStyle.dp(7.5))

    font.pixelSize: VLCStyle.fontSize_normal

    property VLCColors colors: VLCStyle.colors

    readonly property real _x: pos.x - (width / 2)
    property point pos

    contentItem: Label {
        text: pointingTooltip.text
        font: pointingTooltip.font
        color: colors.tooltipTextColor
    }

    background: Rectangle {
        border.color: colors.border
        color: colors.tooltipColor
        radius: VLCStyle.dp(6, VLCStyle.scale)

        Item {
            id: arrowArea

            z: 1
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.bottom
            anchors.topMargin: -(parent.border.width)

            implicitHeight: arrow.implicitHeight * Math.sqrt(2) / 2

            clip: true

            Rectangle {
                id: arrow

                anchors.horizontalCenter: parent.horizontalCenter
                anchors.horizontalCenterOffset: _x - pointingTooltip.x
                anchors.verticalCenter: parent.top

                implicitWidth: VLCStyle.dp(10, VLCStyle.scale)
                implicitHeight: VLCStyle.dp(10, VLCStyle.scale)

                rotation: 45

                color: background.color
                border.color: background.border.color
            }
        }
    }
}
