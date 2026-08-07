#pragma once

#include <cmath>

namespace portable_site {

constexpr double kEarthRadiusMeters = 6378137.0;

struct SiteAnchor {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    // Compass heading of field +X, clockwise from north.
    double heading_degrees = 0.0;
};

struct GeodeticTarget {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude = 0.0;
    double yaw_degrees = 0.0;
};

inline double degreesToRadians(double value) {
    constexpr double kPi = 3.14159265358979323846;
    return value * kPi / 180.0;
}

inline double radiansToDegrees(double value) {
    constexpr double kPi = 3.14159265358979323846;
    return value * 180.0 / kPi;
}

inline double normalizeSignedDegrees(double value) {
    return radiansToDegrees(std::atan2(
        std::sin(degreesToRadians(value)),
        std::cos(degreesToRadians(value))));
}

inline bool isValidAnchor(const SiteAnchor& anchor) {
    return std::isfinite(anchor.latitude) &&
           std::isfinite(anchor.longitude) &&
           std::isfinite(anchor.altitude) &&
           std::isfinite(anchor.heading_degrees) &&
           std::abs(anchor.latitude) <= 90.0 &&
           std::abs(anchor.longitude) <= 180.0 &&
           std::abs(std::cos(degreesToRadians(anchor.latitude))) > 1e-6;
}

inline double horizontalDistanceMeters(const SiteAnchor& anchor,
                                       double latitude,
                                       double longitude) {
    if (!isValidAnchor(anchor) || !std::isfinite(latitude) ||
        !std::isfinite(longitude)) {
        return INFINITY;
    }
    const double mean_latitude =
        degreesToRadians((anchor.latitude + latitude) / 2.0);
    const double east = degreesToRadians(longitude - anchor.longitude) *
                        kEarthRadiusMeters * std::cos(mean_latitude);
    const double north = degreesToRadians(latitude - anchor.latitude) *
                         kEarthRadiusMeters;
    return std::hypot(east, north);
}

inline bool isLocalTargetWithinEnvelope(double field_x,
                                        double field_z,
                                        double relative_altitude,
                                        double horizontal_radius,
                                        double maximum_altitude) {
    return std::isfinite(field_x) && std::isfinite(field_z) &&
           std::isfinite(relative_altitude) &&
           std::isfinite(horizontal_radius) &&
           std::isfinite(maximum_altitude) &&
           horizontal_radius > 0.0 && maximum_altitude > 0.0 &&
           std::hypot(field_x, field_z) <= horizontal_radius &&
           relative_altitude >= 0.0 &&
           relative_altitude <= maximum_altitude;
}

inline GeodeticTarget targetFromField(const SiteAnchor& anchor,
                                      double field_x,
                                      double field_z,
                                      double relative_altitude,
                                      double yaw_offset_degrees) {
    const double heading = degreesToRadians(anchor.heading_degrees);

    // Field +X follows the launch heading; field +Z points left.
    const double east = field_x * std::sin(heading) -
                        field_z * std::cos(heading);
    const double north = field_x * std::cos(heading) +
                         field_z * std::sin(heading);
    const double latitude = anchor.latitude +
        radiansToDegrees(north / kEarthRadiusMeters);
    const double longitude = anchor.longitude + radiansToDegrees(
        east / (kEarthRadiusMeters *
                std::cos(degreesToRadians(anchor.latitude))));

    return GeodeticTarget{
        latitude,
        longitude,
        anchor.altitude + relative_altitude,
        normalizeSignedDegrees(anchor.heading_degrees + yaw_offset_degrees),
    };
}

inline bool isPositionWithinEnvelope(const SiteAnchor& anchor,
                                     double latitude,
                                     double longitude,
                                     double altitude,
                                     double horizontal_radius,
                                     double minimum_relative_altitude,
                                     double maximum_relative_altitude) {
    if (!std::isfinite(altitude) ||
        !std::isfinite(minimum_relative_altitude) ||
        !std::isfinite(maximum_relative_altitude) ||
        minimum_relative_altitude > maximum_relative_altitude) {
        return false;
    }
    const double relative_altitude = altitude - anchor.altitude;
    return horizontalDistanceMeters(anchor, latitude, longitude) <=
               horizontal_radius &&
           relative_altitude >= minimum_relative_altitude &&
           relative_altitude <= maximum_relative_altitude;
}

}  // namespace portable_site
