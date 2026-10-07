#pragma once

#include <mln/tile/geometry_tile_data.hpp>

namespace mln {
namespace util {

bool polygonIntersectsBufferedMultiPoint(const GeometryCoordinates&, const GeometryCollection&, float radius);
bool polygonIntersectsBufferedMultiLine(const GeometryCoordinates&, const GeometryCollection&, float radius);
bool polygonIntersectsBufferedMultiLine(const GeometryCoordinates&,
                                        const GeometryCollection&,
                                        const std::vector<std::vector<float>>& radii);
bool polygonIntersectsPolygon(const GeometryCoordinates&, const GeometryCoordinates&);
bool polygonIntersectsMultiPolygon(const GeometryCoordinates&, const GeometryCollection&);
bool polygonIntersectsBufferedPoint(const GeometryCoordinates& polygon, const GeometryCoordinate& point, float radius);

} // namespace util
} // namespace mln
