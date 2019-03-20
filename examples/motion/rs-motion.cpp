// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2019 Intel Corporation. All Rights Reserved.
#include <librealsense2/rs.hpp>
#include <mutex>
#include "example.hpp"          // Include short list of convenience functions for rendering
#include <cstring>
#include <chrono>
#include <thread>

struct short3
{
    uint16_t x, y, z;
};

#include "d435.h"
#include "t265.h"

enum {
    ERROR,
    IMU,
    POSE,
};

void draw_axes()
{
    glLineWidth(2);
    glBegin(GL_LINES);
    // Draw x, y, z axes
    glColor3f(1, 0, 0); glVertex3f(0, 0, 0);  glVertex3f(-1, 0, 0);
    glColor3f(0, 1, 0); glVertex3f(0, 0, 0);  glVertex3f(0, -1, 0);
    glColor3f(0, 0, 1); glVertex3f(0, 0, 0);  glVertex3f(0, 0, 1);
    glEnd();

    glLineWidth(1);
}

void draw_floor()
{
    glBegin(GL_LINES);
    glColor4f(0.4f, 0.4f, 0.4f, 1.f);
    // Render "floor" grid
    for (int i = 0; i <= 8; i++)
    {
        glVertex3i(i - 4, 1, 0);
        glVertex3i(i - 4, 1, 8);
        glVertex3i(-4, 1, i);
        glVertex3i(4, 1, i);
    }
    glEnd();
}

void render_scene(glfw_state app_state)
{
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glColor3f(1.0, 1.0, 1.0);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0, 4.0 / 3.0, 1, 40);

    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);

    glLoadIdentity();
    gluLookAt(1, 0, 5, 1, 0, 0, 0, -1, 0);

    glTranslatef(0, 0, +0.5f + app_state.offset_y*0.05f);
    glRotated(app_state.pitch, -1, 0, 0);
    glRotated(app_state.yaw, 0, 1, 0);
    draw_floor();
}

class camera_renderer
{
    std::vector<float3> positions, normals;
    std::vector<short3> indexes;
public:
    // Initialize renderer with data needed to draw the camera
    camera_renderer(int stream)
    {
        if (stream == IMU)
            uncompress_d435_obj(positions, normals, indexes);
        else
            uncompress_t265_obj(positions, normals, indexes);
    }

    void draw()
    {
        draw_axes();
        // Scale camera drawing
        glScalef(0.01, 0.01, 0.01);

        glBegin(GL_TRIANGLES);
        // Draw the camera
        for (auto& i : indexes)
        {
            glVertex3fv(&positions[i.x].x);
            glVertex3fv(&positions[i.y].x);
            glVertex3fv(&positions[i.z].x);
            glColor4f(0.05f, 0.05f, 0.05f, 0.3f);
        }
        glEnd();
    }

    // Takes rotation angle as input and rotates the 3D camera model accordignly (used for D435i)
    void render_camera (float3 theta)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        // Set the rotation, converting theta to degrees
        glRotatef(theta.x * 180 / PI, 0, 0, -1);
        glRotatef(theta.y * 180 / PI, 0, -1, 0);
        glRotatef((theta.z - PI / 2) * 180 / PI, -1, 0, 0);
        draw();
        glDisable(GL_BLEND);
        glFlush();
    }

    // Takes a transformation matrix and applies it to the 3D camera model (used for T265)
    void render_camera (float r[16])
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glRotatef(180, 1, 0, 0);
        // Set the transformation
        glMultMatrixf(r);
        draw();
        glDisable(GL_BLEND);
        glFlush();
    }
};

// Class for calculating rotation angle of the D435i device, note that T265 has built-in pose stream and therefore this class is not needed
class rotation_estimator
{
    // theta is the angle of camera rotation in x, y and z components
    float3 theta;
    std::mutex theta_mtx;
    /* alpha indicates the part that gyro and accelerometer take in computation of theta; higher alpha gives more weight to gyro, but too high
    values cause drift; lower alpha gives more weight to accelerometer, which is more sensitive to disturbances */
    float alpha = 0.98;
    bool first = true;
    // Keeps the arrival time of previous gyro frame
    double last_ts_gyro = 0;
public:
    // Function to calculate the change in angle of motion based on data from gyro
    void process_gyro(rs2_vector gyro_data, double ts)
    {
        if (first) // On the first iteration, use only data from accelerometer to set the camera's initial position
        {
            last_ts_gyro = ts;
            return;
        }
        // Holds the change in angle, as calculated from gyro
        float3 gyro_angle;

        // Initialize gyro_angle with data from gyro
        gyro_angle.x = gyro_data.x; // Pitch
        gyro_angle.y = gyro_data.y; // Yaw
        gyro_angle.z = gyro_data.z; // Roll

        // Compute the difference between arrival times of previous and current gyro frames
        double dt_gyro = (ts - last_ts_gyro) / 1000.0;
        last_ts_gyro = ts;

        // Change in angle equals gyro measures * time passed since last measurement
        gyro_angle = gyro_angle * dt_gyro;

        // Apply the calculated change of angle to the current angle (theta)
        std::lock_guard<std::mutex> lock(theta_mtx);
        theta.add(-gyro_angle.z, -gyro_angle.y, gyro_angle.x);
    }

    void process_accel(rs2_vector accel_data)
    {
        // Holds the angle as calculated from accelerometer data
        float3 accel_angle;

        // Calculate rotation angle from accelerometer data
        accel_angle.z = atan2(accel_data.y, accel_data.z);
        accel_angle.x = atan2(accel_data.x, sqrt(accel_data.y * accel_data.y + accel_data.z * accel_data.z));

        // If it is the first iteration, set initial pose of camera according to accelerometer data (note the different handling for Y axis)
        std::lock_guard<std::mutex> lock(theta_mtx);
        if (first)
        {
            first = false;
            theta = accel_angle;
            // Since we can't infer the angle around Y axis using accelerometer data, we'll use PI as a convetion for the initial pose
            theta.y = PI;
        }
        else
        {
            /* 
            Apply Complementary Filter:
                - high-pass filter = theta * alpha:  allows short-duration signals to pass through while filtering out signals
                  that are steady over time, is used to cancel out drift.
                - low-pass filter = accel * (1- alpha): lets through long term changes, filtering out short term fluctuations 
            */
            theta.x = theta.x * alpha + accel_angle.x * (1 - alpha);
            theta.z = theta.z * alpha + accel_angle.z * (1 - alpha);
        }
    }
    
    // Returns the current rotation angle
    float3 get_theta()
    {
        std::lock_guard<std::mutex> lock(theta_mtx);
        return theta;
    }
};


// For T265, we can calculate transformation matrix using pose data
void calc_transform(rs2_pose& pose_data, float mat[16])
{
    auto q = pose_data.rotation;
    auto t = pose_data.translation;
    // Set the matrix as column-major for convenient work with OpenGL, rotate 180 degrees in y axia (negating 1st and 3rd columns)
    mat[0] = -(1 - 2 * q.y*q.y - 2 * q.z*q.z); mat[4] = (2 * q.x*q.y - 2 * q.z*q.w);     mat[8] = -(2 * q.x*q.z + 2 * q.y*q.w);      mat[12] = t.x;
    mat[1] = -(2 * q.x*q.y + 2 * q.z*q.w);     mat[5] = (1 - 2 * q.x*q.x - 2 * q.z*q.z); mat[9] = -(2 * q.y*q.z - 2 * q.x*q.w);      mat[13] = t.y;
    mat[2] = -(2 * q.x*q.z - 2 * q.y*q.w);     mat[6] = (2 * q.y*q.z + 2 * q.x*q.w);     mat[10] = -(1 - 2 * q.x*q.x - 2 * q.y*q.y); mat[14] = t.z;
    mat[3] = 0.0f;                             mat[7] = 0.0f;                            mat[11] = 0.0f;                             mat[15] = 1.0f;
}


int check_supported_stream()
{
    bool found_gyro = false;
    bool found_accel = false;
    rs2::context ctx;
    ctx.query_devices();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    for (auto dev : ctx.query_devices())
    {
        // The same device should support gyro and accel
        found_gyro = false;
        found_accel = false;
        for (auto sensor : dev.query_sensors())
        {
            for (auto profile : sensor.get_stream_profiles())
            {
                if (profile.stream_type() == RS2_STREAM_POSE)
                   return POSE;

                if (profile.stream_type() == RS2_STREAM_GYRO)
                    found_gyro = true;

                if (profile.stream_type() == RS2_STREAM_ACCEL)
                    found_accel = true;
            }
        }
        if (found_gyro && found_accel)
            return IMU;
    }
    return ERROR;
}

int main(int argc, char * argv[]) try
{
    // Before running the example, check that a device supporting IMU or pose is connected
    int stream = check_supported_stream();
    if (stream == ERROR)
    {
        std::cerr << "Device supporting IMU (D435i) or pose stream (T265) not found";
        return EXIT_FAILURE;
    }
    // Initialize window for rendering
    window app(1280, 720, "RealSense Motion Example");
    // Construct an object to manage view state
    glfw_state app_state(0.0, 0.0);
    // Register callbacks to allow manipulation of the view state
    register_glfw_callbacks(app, app_state);

    // Declare RealSense pipeline, encapsulating the actual device and sensors
    rs2::pipeline pipe;
    // Create a configuration for configuring the pipeline with a non default profile
    rs2::config cfg;
    // Declare object for rendering camera motion and initialize it with the available stream
    camera_renderer camera(stream);

    // D435i
    if (stream == IMU)
    {
        // Declare object that handles camera pose calculations; We'll use it only for D435i, since T265 has built-in pose stream
        rotation_estimator algo;

        // For D435i, add streams of gyro and accelerometer to configuration
        cfg.enable_stream(RS2_STREAM_ACCEL, RS2_FORMAT_MOTION_XYZ32F);
        cfg.enable_stream(RS2_STREAM_GYRO, RS2_FORMAT_MOTION_XYZ32F);

        // start streaming with the given configuration;
        // Note that since we only allow IMU streams, only single frames are produced
        auto profile = pipe.start(cfg, [&](rs2::frame frame)
        {
            // Cast the frame that arrived to motion frame
            auto motion = frame.as<rs2::motion_frame>();
            // If casting succeeded and the arrived frame is from gyro stream
            if (motion && motion.get_profile().stream_type() == RS2_STREAM_GYRO && motion.get_profile().format() == RS2_FORMAT_MOTION_XYZ32F)
            {
                // Get the timestamp of the current frame
                double ts = motion.get_timestamp();
                // Get gyro measures
                rs2_vector gyro_data = motion.get_motion_data();
                // Call function that computes the angle of motion based on the retrieved measures
                algo.process_gyro(gyro_data, ts);
            }
            // If casting succeeded and the arrived frame is from accelerometer stream
            if (motion && motion.get_profile().stream_type() == RS2_STREAM_ACCEL && motion.get_profile().format() == RS2_FORMAT_MOTION_XYZ32F)
            {
                // Get accelerometer measures
                rs2_vector accel_data = motion.get_motion_data();
                // Call function that computes the angle of motion based on the retrieved measures
                algo.process_accel(accel_data);
            }
        });
        // Main loop
        while (app)
        {
            // Configure scene, draw floor, handle manipultation by the user etc.
            render_scene(app_state);
            // Draw the camera according to the computed theta
            camera.render_camera(algo.get_theta());
        }
    }
    else // T265
    {
        // Add pose stream (available for T265)
        cfg.enable_stream(RS2_STREAM_POSE, RS2_FORMAT_6DOF);
        // Start pipeline with chosen configuration
        pipe.start(cfg);

        // Main loop
        while (app)
        {
            auto frames = pipe.wait_for_frames();
            // Get a frame from the pose stream
            auto f = frames.first_or_default(RS2_STREAM_POSE);
            auto pose_data = f.as<rs2::pose_frame>().get_pose_data();
            float r[16];
            // Calculate current transformation matrix
            calc_transform(pose_data, r);
            // Configure scene, draw floor, handle manipultation by the user etc.
            render_scene(app_state);
            camera.render_camera(r);
        }
    }

    // Stop the pipeline
    pipe.stop();

    return EXIT_SUCCESS;
}
catch (const rs2::error & e)
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args() << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}
