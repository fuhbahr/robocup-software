#include <rrt/planning/Path.hpp>
#include <planning/trajectory/PathSmoothing.hpp>
#include <planning/trajectory/VelocityProfiling.hpp>
#include "PathTargetPlanner.hpp"
#include "planning/trajectory/Trajectory.hpp"
#include "planning/trajectory/RRTUtil.hpp"
#include <Geometry2d/Pose.hpp>
#include <vector>

namespace Planning {

REGISTER_CONFIGURABLE(PathTargetPlanner);

void PathTargetPlanner::createConfiguration(Configuration* cfg) {
    _partialReplanLeadTime = new ConfigDouble(
            cfg, "PathTargetPlanner/partialReplanLeadTime", 0.2, "partialReplanLeadTime");
}

Trajectory PathTargetPlanner::plan(Planning::PlanRequest &&request) {
    using Geometry2d::Point;
    using Geometry2d::Pose;
    using Geometry2d::Twist;

    Trajectory result = std::move(request.prevTrajectory);

    RobotInstant start_instant;
    start_instant.pose = request.start.pose;
    start_instant.velocity = request.start.velocity;
    start_instant.stamp = request.start.timestamp;

    //assumes the robot is on the path
    if (!result.empty()) {
        auto maybe_start = result.evaluate(RJ::now());
        if (maybe_start) {
            start_instant = *maybe_start;
        }
    }

    PathTargetCommand command = std::get<PathTargetCommand>(request.motionCommand);

    auto state_space = std::make_shared<RoboCupStateSpace>(
            Field_Dimensions::Current_Dimensions, std::move(request.obstacles));

    // Simple case: no path
    const Pose& start_pose = start_instant.pose;
    const Pose& goal_pose = command.pathGoal.pose;
    if (start_pose.position() == goal_pose.position()) {
        std::vector<RobotInstant> instants;
        instants.push_back(RobotInstant(start_pose, Twist(), RJ::now()));
        result = std::move(Trajectory(std::move(instants)));
        //todo(Ethan) fix this
//        result.setDebugText("Invalid Basic Path");
        return std::move(result);
    }

    auto rrt = GenerateRRT(start_pose.position(), goal_pose.position(), state_space);

    if (rrt.empty()) {
        return Trajectory({});
    }

    // Make the path smooth
    RRT::SmoothPath(rrt, *state_space);


    const RJ::Seconds partialReplanTime(*_partialReplanLeadTime);

    enum ReplanState { Reuse, FullReplan, PartialReplan, CheckBetter };

    BezierPath path(rrt, start_instant.velocity.linear(), command.pathGoal.velocity.linear(), request.constraints.mot);
    result = ProfileVelocity(path,
                             start_instant.velocity.linear().mag(),
                             command.pathGoal.velocity.linear().mag(),
                             request.constraints.mot);

    // Draw the trajectory
    result.draw(&(request.context->debug_drawer));

    std::function<double(Point, Point, double)> angleFunction =
            [](Point pos, Point vel_linear, double angle) -> double {
                // Find the nearest angle either matching velocity or at a 180 degree angle.
                double angle_delta = fixAngleRadians(vel_linear.angle() - angle);
                if (std::abs(angle_delta) < 90) {
                    return angle + angle_delta;
                } else {
                    return fixAngleRadians(angle + angle_delta + M_PI);
                }
            };
    PlanAngles(result, RobotState{start_instant.pose, start_instant.velocity, start_instant.stamp}, angleFunction, request.constraints.rot);
    return result;
}

} // namespace Planning
