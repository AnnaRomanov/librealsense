# rs-motion Sample

## Overview
This sample demonstrates how to use measures from gyroscope and accelerometer to compute the rotation angle, `theta`, of the camera.
The example is based on [code](https://github.com/GruffyPuffy/imutest) by GruffyPuffy.

## Expected Output
The application should open a window with a 3D image of the camera, rotating according to it's physical motion. In addition, you should be able to interact with the camera using your mouse, rotating, zooming, and panning.

## Code Overview

First, we include the Intel® RealSense™ Cross-Platform API.  
All but advanced functionality is provided through a single header:
```cpp
#include <librealsense2/rs.hpp> // Include RealSense Cross Platform API
```

In this example we will also use the auxiliary library of `example.hpp`:
```cpp
#include "../example.hpp"    
```

We define 3 auxilary functions: `draw_axes` and `draw_floor`, which draw axes and floor grid respectively; and `render_scene` which prepares the general set up of the scence, handles user manipulation on the camera image etc.

Next, we define an auxilary class for rendering, `camera_renderer`. This class holds 3 vectors: `positions`, `normals` and `indexes`, and they define the 3D model of the camera. `render_camera` is the function that actually draws the camera, rotated by the computed angle `theta`.

Calculation of the rotation angle, based on gyroscope and accelerometer data, is handled by the class `rotation_estimator`.

The class holds `theta` itself, which is updated on each IMU frame. We will process IMU frame asynchronously and therefore a mutex `theta_mtx` is needed.

`rotation_estimator` also holds the parameter `alpha`, which is used to aggregate both gyroscope and accelerometer data in the calculation of theta.

The first iteration needs separate handling and therfore `first` is defined.

`last_ts_gyro` is needed to calculate the time elapsed between arrival of subsequent frames from gyroscope stream.
```cpp
// theta is the angle of camera rotation in x, y and z components
float3 theta;
std::mutex theta_mtx;
/* alpha indicates the part that gyro and accelerometer take in computation of theta; higher alpha gives more weight
to gyro, but too high values cause drift; lower alpha gives more weight to accelerometer, which is more sensitive to
disturbances */
float alpha = 0.98;
bool first = true;
// Keeps the arrival time of previous gyro frame
double last_ts_gyro = 0;
```


The function `process_gyro` computes the change in rotation angle, based on gyroscope measures. It accepts `gyro_data`, an `rs2_vector` containing measures retrieved from gyroscope, and `ts`, the timestamp of the previous frame from gyroscope.

In order to compute the change in direction of motion, we multiply gyroscope measures by the length of the time period they correspond to. Then, we add or substract the retrieved difference from the current theta. The sign is determined by the defined axis positive direction.

Note that gyroscope data is ignored until accelerometer data arrives, since the initial position is retrieved from the accelerometer (and afterwards, gyroscope allows us to calculate the direction of motion ).

```cpp
void process_gyro(rs2_vector gyro_data, double ts)
{
    if (first) // On the first iteration, use only data from accelerometer to set the camera's initial position
        return;
    // Holds the change in angle, as calculated from gyro
    float3 gyro_angle;

    // Multiply the gyro measures by constants to fit them to angles in reality
    gyro_angle.x = gyro_data.x * 0.5; // Pitch
    gyro_angle.y = gyro_data.y * 0.3; // Yaw
    gyro_angle.z = gyro_data.z * 0.5; // Roll

    // Compute the difference between arrival times of previous and current gyro frames
    double dt_gyro = (ts - last_ts_gyro) / 1000.0;
    last_ts_gyro = ts;

    // Change in angle equals gyro measures * time passed since last measurement
    gyro_angle = gyro_angle * dt_gyro;

    // Apply the calculated change of angle to the current angle (theta)
    std::lock_guard<std::mutex> lock(theta_mtx);
    theta.x -= gyro_angle.z;
    theta.y -= gyro_angle.y;
    theta.z += gyro_angle.x;
}
```


The function `process_accel` computes the rotation angle from accelerometer data and updates the current theta. `accel_data` is and `rs2_vector` that hold the measures retreived from the accelerometer stream.

The angles in the z and x axis are computed from acclerometer data, using trigonometric calculations. Note that motion around Y axis cannot be estimated using accelerometer.
```cpp
void process_accel(rs2_vector accel_data)
{
    // Holds the angle as calculated from accelerometer data
    float3 accel_angle;
    // Calculate rotation angle from accelerometer data
    accel_angle.z = atan2(accel_data.y, accel_data.z);
    accel_angle.x = atan2(accel_data.x, sqrt(accel_data.y * accel_data.y + accel_data.z * accel_data.z));
```


If it is the first time we handle accelerometer data, we intiailize `theta` with the angles computed above. `theta` in Y direction is set to `PI`, facing the user.
```cpp
    // If it is the first iteration, set initial pose of camera according to accelerometer data
    // (note the different handling for Y axis)
    std::lock_guard<std::mutex> lock(theta_mtx);
    if (first)
    {
        first = false;
        theta = accel_angle;
        // Since we can't infer the angle around Y axis using accelerometer data, we'll use PI as a convetion for
        // the initial pose
        theta.y = PI;
    }
```


Otherwise, we use an approximate version of Complementary Filter to balance gyroscope and accelerometer results. Gyroscope gives generally accurate motion data, but it tends to drift, not returning to zero when the system went back to its original position. Accelerometer, on the other hand, doesn't have this problem, but it's signals are not as smooth and are easily affected by noises and disturbances. We use `alpha` to aggregate the data. New `theta` is a combination of the previous angle adjusted by difference computed from gyroscope measures, with the angle calculated from accelerometer measures. Note that this calculation does not apply to the Y component of `theta`, since it is measured by gyroscope only.
```cpp
        else
        {
            /* 
            Apply Complementary Filter:
                - "high-pass filter" = theta * alpha:  allows short-duration signals to pass through while filtering
                  out signals that are steady over time, is used to cancel out drift.
                - "low-pass filter" = accel * (1- alpha): lets through long term changes, filtering out short term
                  fluctuations 
            */
            theta.x = theta.x * alpha + accel_angle.x * (1 - alpha);
            theta.z = theta.z * alpha + accel_angle.z * (1 - alpha);
        }
    }
```


The main function handels frames arriving from IMU streams.
First, declare pipeline and configure it with `RS2_STREAM_ACCEL` and `RS2_STREAM_GYRO`.
```cpp
// Declare RealSense pipeline, encapsulating the actual device and sensors
rs2::pipeline pipe;
// Create a configuration for configuring the pipeline with a non default profile
rs2::config cfg;

// Add streams of gyro and accelerometer to configuration
cfg.enable_stream(RS2_STREAM_ACCEL, RS2_FORMAT_MOTION_XYZ32F);
cfg.enable_stream(RS2_STREAM_GYRO, RS2_FORMAT_MOTION_XYZ32F);
```

Next, we declare objects of the helper classes we defined above.
```cpp
// Declare object for rendering camera motion
camera_renderer camera;
// Declare object that handles camera pose calculations
rotation_estimator algo;
```


To process frames from gyroscope and accelerometer streams asynchronously, we'll use pipeline's callback. When a frame arrives, we cast it to `rs2::motion_frame`. If it's a gyro frame, we find its timestamp using `motion.get_timestamp()`. Then, we can get its IMU data and call the matching function to calculate rotation angle.
```cpp
auto profile = pipe.start(cfg, [&](rs2::frame frame)
{
    // Cast the frame that arrived to motion frame
    auto motion = frame.as<rs2::motion_frame>();
    // If casting succeeded and the arrived frame is from gyro stream
    if (motion && motion.get_profile().stream_type() == RS2_STREAM_GYRO && 
    	motion.get_profile().format() == RS2_FORMAT_MOTION_XYZ32F)
    {
        // Get the timestamp of the current frame
        double ts = motion.get_timestamp();
        // Get gyro measures
        rs2_vector gyro_data = motion.get_motion_data();
        // Call function that computes the angle of motion based on the retrieved measures
        algo.process_gyro(gyro_data, ts);
    }
    // If casting succeeded and the arrived frame is from accelerometer stream
    if (motion && motion.get_profile().stream_type() == RS2_STREAM_ACCEL && 
    	motion.get_profile().format() == RS2_FORMAT_MOTION_XYZ32F)
    {
        // Get accelerometer measures
        rs2_vector accel_data = motion.get_motion_data();
        // Call function that computes the angle of motion based on the retrieved measures
        algo.process_accel(accel_data);
    }
});
```


The main loop renders the camera, retrieving the current `theta` in each iteration.
```cpp
while (app)
{
    // Configure scene, draw floor, handle manipultation by the user etc.
    render_scene(app_state);
    // Draw the camera according to the computed theta
    camera.render_camera(algo.get_theta());
}
```