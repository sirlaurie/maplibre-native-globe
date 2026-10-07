#pragma once

#include <mln/map/projection_base.hpp>
#include <mln/map/transform_state.hpp>
#include <mln/map/vertical_perspective_projection.hpp>
#include <mln/tile/tile_id.hpp>

#include <cmath>
#include <numbers>

namespace mln {

/// Projects the points of one tile through the current projection, the way the vertex shaders do it.
class TileProjector {
public:
    TileProjector(const TransformState& state_, const UnwrappedTileID& tileID_)
        : TileProjector(state_, tileID_, state_.getProjectionData(tileID_)) {}

    TileProjector(const TransformState& state_, const UnwrappedTileID& tileID_, ProjectionData data_)
        : state(&state_),
          tileID(tileID_),
          data(std::move(data_)) {}

    ProjectedTilePoint project(const Point<double>& point, double elevation = 0.0) const {
        return state->getProjection().projectTilePoint(data, tileID, point, elevation);
    }

    vec4 projectSphere(const Point<double>& tilePoint, const vec3& sphere, double elevation = 0.0) const {
        return VerticalPerspectiveProjection::projectSphere(data, tilePoint, sphere, elevation);
    }

    double circleRadiusCorrection() const { return state->getProjection().circleRadiusCorrection(*state); }

    double lineThicknessCorrection(double tileY) const {
        const double mercatorY = data.tileMercatorCoords[1] + data.tileMercatorCoords[3] * tileY;
        const double thickness = std::cosh(std::numbers::pi * (1.0 - 2.0 * mercatorY));
        const float transition = static_cast<float>(data.projectionTransition);
        return transition < 0.999f ? std::lerp(1.0, thickness, transition) : thickness;
    }

    double pitchedTextCorrection(const Point<double>& tileAnchor) const {
        return state->getProjection().pitchedTextCorrection(*state, tileAnchor, tileID);
    }

    const TransformState& getTransformState() const { return *state; }
    const UnwrappedTileID& getTileID() const { return tileID; }
    const ProjectionData& getProjectionData() const { return data; }

private:
    const TransformState* state;
    UnwrappedTileID tileID;
    ProjectionData data;
};

} // namespace mln
