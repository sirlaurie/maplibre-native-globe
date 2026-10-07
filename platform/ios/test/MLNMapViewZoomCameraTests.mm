#import <Mapbox.h>

#if MLN_RENDER_BACKEND_METAL

#import <Metal/Metal.h>
#import <XCTest/XCTest.h>
#import "MLNMapView_Private.h"
#import "MLNMockGestureRecognizers.h"

#include <mln/map/camera.hpp>
#include <mln/map/map.hpp>

@interface MLNMapView (MLNMapViewZoomCameraTests)
- (void)handlePinchGesture:(UIPinchGestureRecognizer *)pinch;
- (MLNMapCamera *)cameraByZoomingToZoomLevel:(double)zoom aroundAnchorPoint:(CGPoint)anchorPoint;
@end

@interface MLNZoomCameraDelegate : NSObject <MLNMapViewDelegate>
@property (nonatomic) BOOL allowsChange;
@property (nonatomic) NSUInteger callCount;
@property (nonatomic) MLNMapCamera *oldCamera;
@property (nonatomic) MLNMapCamera *proposedCamera;
@end

@implementation MLNZoomCameraDelegate

- (BOOL)mapView:(MLNMapView *)mapView
    shouldChangeFromCamera:(MLNMapCamera *)oldCamera
                  toCamera:(MLNMapCamera *)newCamera
                    reason:(MLNCameraChangeReason)reason {
  self.callCount += 1;
  if (!self.oldCamera) {
    self.oldCamera = oldCamera;
    self.proposedCamera = newCamera;
  }
  return self.allowsChange;
}

@end

@interface MLNLegacyZoomCameraDelegate : NSObject <MLNMapViewDelegate>
@property (nonatomic) MLNMapCamera *proposedCamera;
@end

@implementation MLNLegacyZoomCameraDelegate

- (BOOL)mapView:(MLNMapView *)mapView
    shouldChangeFromCamera:(MLNMapCamera *)oldCamera
                  toCamera:(MLNMapCamera *)newCamera {
  if (!self.proposedCamera) {
    self.proposedCamera = newCamera;
  }
  return YES;
}

@end

@interface MLNMapViewZoomCameraTests : XCTestCase <MLNMapViewDelegate>
@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) UIWindow *window;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;
@end

@implementation MLNMapViewZoomCameraTests

- (void)setUp {
  [super setUp];

  XCTSkipIf(MTLCreateSystemDefaultDevice() == nil, @"Metal is unavailable on this device.");

  self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 390, 874)
                                         styleJSON:@"{\"version\":8,\"sources\":{},\"layers\":[]}"];
  self.mapView.automaticallyAdjustsContentInset = NO;
  self.mapView.contentInset = UIEdgeInsetsZero;
  self.mapView.delegate = self;

  UIViewController *controller = [[UIViewController alloc] init];
  [controller.view addSubview:self.mapView];
  self.window = [[UIWindow alloc] initWithFrame:self.mapView.bounds];
  self.window.rootViewController = controller;
  [self.window makeKeyAndVisible];

  if (!self.mapView.style) {
    self.styleLoadingExpectation = [self expectationWithDescription:@"Style loaded"];
    [self waitForExpectations:@[ self.styleLoadingExpectation ] timeout:10];
  }

  XCTAssertNotNil(self.mapView.style);
}

- (void)tearDown {
  self.styleLoadingExpectation = nil;
  self.mapView.delegate = nil;
  self.window.hidden = YES;
  self.window.rootViewController = nil;
  self.window = nil;
  self.mapView = nil;

  [super tearDown];
}

- (void)mapView:(MLNMapView *)mapView didFinishLoadingStyle:(MLNStyle *)style {
  [self.styleLoadingExpectation fulfill];
  self.styleLoadingExpectation = nil;
}

- (void)testGlobePinchDelegateReceivesAppliedCamera {
  [self configureProjection:@"globe" zoom:11.55];
  MLNMapCamera *originalCamera = self.mapView.camera;
  MLNZoomCameraDelegate *delegate = [[MLNZoomCameraDelegate alloc] init];
  delegate.allowsChange = YES;
  self.mapView.delegate = delegate;

  [self pinchToZoom:11.45 aroundAnchorPoint:CGPointMake(260, 340)];

  XCTAssertEqual(delegate.callCount, 2u);
  [self assertCamera:delegate.oldCamera equalsCamera:originalCamera];
  [self assertCamera:delegate.proposedCamera equalsCamera:self.mapView.camera];
}

- (void)testMercatorPinchDelegateReceivesAppliedCamera {
  [self configureProjection:@"mercator" zoom:11.55];
  MLNZoomCameraDelegate *delegate = [[MLNZoomCameraDelegate alloc] init];
  delegate.allowsChange = YES;
  self.mapView.delegate = delegate;

  [self pinchToZoom:11.45 aroundAnchorPoint:CGPointMake(260, 340)];

  XCTAssertEqual(delegate.callCount, 2u);
  [self assertCamera:delegate.proposedCamera equalsCamera:self.mapView.camera];
}

- (void)testGlobePinchDelegateCanVetoCameraChange {
  [self configureProjection:@"globe" zoom:11.55];
  MLNMapCamera *originalCamera = self.mapView.camera;
  MLNZoomCameraDelegate *delegate = [[MLNZoomCameraDelegate alloc] init];
  delegate.allowsChange = NO;
  self.mapView.delegate = delegate;

  [self pinchToZoom:11.45 aroundAnchorPoint:CGPointMake(260, 340)];

  XCTAssertEqual(delegate.callCount, 2u);
  XCTAssertNotNil(delegate.proposedCamera);
  [self assertCamera:self.mapView.camera equalsCamera:originalCamera];
}

- (void)testGlobeLegacyPinchDelegateReceivesAppliedCamera {
  [self configureProjection:@"globe" zoom:11.55];
  MLNLegacyZoomCameraDelegate *delegate = [[MLNLegacyZoomCameraDelegate alloc] init];
  self.mapView.delegate = delegate;

  [self pinchToZoom:11.45 aroundAnchorPoint:CGPointMake(260, 340)];

  [self assertCamera:delegate.proposedCamera equalsCamera:self.mapView.camera];
}

- (void)testNegativeGlobeZoomPreviewPreservesLiveCamera {
  [self configureProjection:@"globe" zoom:13];
  MLNMapCamera *originalCamera = self.mapView.camera;
  CGPoint anchorPoint = CGPointMake(195, 437);

  MLNMapCamera *preview = [self.mapView cameraByZoomingToZoomLevel:-0.8
                                                 aroundAnchorPoint:anchorPoint];

  [self assertCamera:self.mapView.camera equalsCamera:originalCamera];

  self.mapView.mbglMap.jumpTo(mln::CameraOptions().withZoom(-0.8).withAnchor(
      mln::ScreenCoordinate{anchorPoint.x, anchorPoint.y}));

  [self assertCamera:preview equalsCamera:self.mapView.camera];
}

- (void)testGlobePinchWithoutCameraDelegatePerformance {
  [self configureProjection:@"globe" zoom:11.55];

  [self measureBlock:^{
    self.mapView.mbglMap.jumpTo(mln::CameraOptions().withZoom(11.55));
    [self pinchToZoom:11.45 aroundAnchorPoint:CGPointMake(200, 402)];
    XCTAssertEqualWithAccuracy(self.mapView.zoomLevel, 11.45, 1e-6);
  }];
}

- (void)configureProjection:(NSString *)projection zoom:(double)zoom {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:projection];
  self.mapView.mbglMap.jumpTo(mln::CameraOptions()
                                  .withCenter(mln::LatLng{60, 170})
                                  .withZoom(zoom)
                                  .withPitch(45)
                                  .withBearing(30)
                                  .withPadding(mln::EdgeInsets{20, 35, 90, 25}));
}

- (void)pinchToZoom:(double)zoom aroundAnchorPoint:(CGPoint)anchorPoint {
  UIPinchGestureRecognizerMock *pinch = [[UIPinchGestureRecognizerMock alloc] initWithTarget:nil
                                                                                      action:nil];
  [self.mapView addGestureRecognizer:pinch];
  pinch.locationInViewOverride = anchorPoint;
  pinch.state = UIGestureRecognizerStateBegan;
  pinch.scale = 1;
  double initialZoom = self.mapView.zoomLevel;
  [self.mapView handlePinchGesture:pinch];

  pinch.state = UIGestureRecognizerStateChanged;
  pinch.scale = exp2(zoom - initialZoom);
  [self.mapView handlePinchGesture:pinch];

  pinch.state = UIGestureRecognizerStateEnded;
  pinch.velocity = 0;
  [self.mapView handlePinchGesture:pinch];

  [self.mapView removeGestureRecognizer:pinch];
}

- (void)assertCamera:(MLNMapCamera *)camera equalsCamera:(MLNMapCamera *)expected {
  XCTAssertNotNil(camera);
  XCTAssertEqualWithAccuracy(camera.centerCoordinate.latitude, expected.centerCoordinate.latitude,
                             1e-8);
  XCTAssertEqualWithAccuracy(camera.centerCoordinate.longitude, expected.centerCoordinate.longitude,
                             1e-8);
  XCTAssertEqualWithAccuracy(camera.altitude, expected.altitude, expected.altitude * 1e-8);
  XCTAssertEqualWithAccuracy(camera.pitch, expected.pitch, 1e-8);
  XCTAssertEqualWithAccuracy(camera.heading, expected.heading, 1e-8);
  XCTAssertEqualWithAccuracy(camera.roll, expected.roll, 1e-8);
}

@end

#endif
