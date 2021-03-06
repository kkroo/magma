"""
Copyright (c) 2016-present, Facebook, Inc.
All rights reserved.

This source code is licensed under the BSD-style license found in the
LICENSE file in the root directory of this source tree. An additional grant
of patent rights can be found in the PATENTS file in the same directory.
"""

import textwrap
from typing import Optional
from magma.enodebd.exceptions import ConfigurationError


DUPLEX_MAP = {
    '01': 'TDDMode',
    '02': 'FDDMode'
}

BANDWIDTH_RBS_TO_MHZ_MAP = {
    'n6': 1.4,
    'n15': 3,
    'n25': 5,
    'n50': 10,
    'n75': 15,
    'n100': 20,
}


def duplex_mode(value: str) -> Optional[str]:
    return DUPLEX_MAP.get(value)


def band_capability(value: str) -> str:
    return ','.join([str(int(b, 16)) for b in textwrap.wrap(value, 2)])


def gps_tr181(value: str) -> str:
    """Convert GPS value (lat or lng) to float

    Per TR-181 specification, coordinates are returned in degrees,
    multiplied by 1,000,000.

    Args:
        value (string): GPS value (latitude or longitude)
    Returns:
        str: GPS value (latitude/longitude) in degrees
    """
    try:
        return str(float(value) / 1e6)
    except ValueError:
        return value


def bandwidth(bandwidth_rbs: str) -> float:
    """

    Map bandwidth in number of RBs to MHz
    TODO: TR-196 spec says this should be '6' rather than 'n6', but
    BaiCells eNodeB uses 'n6'. Need to resolve this.

    Args:
        bandwidth_rbs (str): Bandwidth in number of RBs
    Returns:
        str: Bandwidth in MHz
    """
    if bandwidth_rbs not in BANDWIDTH_RBS_TO_MHZ_MAP:
        raise ConfigurationError('Unknown bandwidth_rbs (%s)' %
                                 str(bandwidth_rbs))
    return BANDWIDTH_RBS_TO_MHZ_MAP[bandwidth_rbs]
