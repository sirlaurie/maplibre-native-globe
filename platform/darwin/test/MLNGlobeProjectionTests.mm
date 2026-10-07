#import <Mapbox.h>
#import <XCTest/XCTest.h>

#if TARGET_OS_IPHONE && MLN_RENDER_BACKEND_METAL

#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#import "MLNMapView_Private.h"

#include <cmath>
#include <mln/map/map.hpp>

@interface MLNGlobeDrawingProbe : MLNCustomStyleLayer
@property (nonatomic, copy) void (^preDrawHandler)(MLNStyleLayerDrawingContext);
@property (nonatomic, copy) void (^drawHandler)(MLNStyleLayerDrawingContext);
@end

@implementation MLNGlobeDrawingProbe

- (void)preDrawInMapView:(MLNMapView *)mapView withContext:(MLNStyleLayerDrawingContext)context {
  if (self.preDrawHandler) {
    self.preDrawHandler(context);
  }
}

- (void)drawInMapView:(MLNMapView *)mapView withContext:(MLNStyleLayerDrawingContext)context {
  if (self.drawHandler) {
    self.drawHandler(context);
  }
}

@end

@interface MLNGlobeProjectionTests : XCTestCase <MLNMapViewDelegate>
@property (nonatomic) MLNMapView *mapView;
@property (nonatomic) UIWindow *window;
@property (nonatomic) XCTestExpectation *styleLoadingExpectation;
@end

@implementation MLNGlobeProjectionTests

- (void)setUp {
  [super setUp];

  XCTSkipIf(MTLCreateSystemDefaultDevice() == nil, @"Metal is unavailable on this device.");

  self.mapView = [[MLNMapView alloc] initWithFrame:CGRectMake(0, 0, 256, 256)
                                         styleJSON:@"{\"version\":8,\"sources\":{},\"layers\":[]}"];
  self.mapView.automaticallyAdjustsContentInset = NO;
  self.mapView.contentInset = UIEdgeInsetsZero;
  self.mapView.delegate = self;
  self.mapView.centerCoordinate = CLLocationCoordinate2DMake(0, 0);
  self.mapView.zoomLevel = 1;

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

- (void)testUnspecifiedProjectionReturnsNil {
  XCTAssertNil(self.mapView.style.projection);
}

- (void)testNamedProjectionsRoundTrip {
  for (NSString *name in @[ @"mercator", @"globe", @"vertical-perspective" ]) {
    self.mapView.style.projection = [NSExpression expressionForConstantValue:name];

    XCTAssertEqualObjects(self.mapView.style.projection.constantValue, name);
  }
}

- (void)testProjectionBlendRoundTrips {
  NSArray *blend = @[ @"vertical-perspective", @"mercator", @0.25 ];
  self.mapView.style.projection = [NSExpression expressionForConstantValue:blend];

  XCTAssertEqualObjects(self.mapView.style.projection.constantValue, blend);
}

- (void)testZoomProjectionExpressionRoundTrips {
  self.mapView.style.projection = [self zoomProjectionExpression];
  NSExpression *readback = self.mapView.style.projection;

  XCTAssertNotNil(readback);
  XCTAssertEqual(readback.expressionType, NSFunctionExpressionType);

  self.mapView.style.projection = nil;
  self.mapView.style.projection = readback;

  XCTAssertEqualObjects(self.mapView.style.projection, readback);
}

- (void)testClearingProjectionRestoresUnspecifiedDefault {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];
  self.mapView.style.projection = nil;

  XCTAssertNil(self.mapView.style.projection);
}

- (void)testInvalidProjectionPreservesCurrentProjection {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];

  XCTAssertThrowsSpecificNamed(
      self.mapView.style.projection = [NSExpression expressionForConstantValue:@"invalid"],
      NSException, NSInvalidArgumentException);
  XCTAssertEqualObjects(self.mapView.style.projection.constantValue, @"globe");
}

- (void)testFeatureDependentProjectionPreservesCurrentProjection {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];

  XCTAssertThrowsSpecificNamed(
      self.mapView.style.projection = [NSExpression expressionForKeyPath:@"projection"],
      NSException, NSInvalidArgumentException);
  XCTAssertEqualObjects(self.mapView.style.projection.constantValue, @"globe");
}

- (void)testProjectionChangePreservesRuntimeStyleContents {
  MLNStyle *style = self.mapView.style;
  MLNPointFeature *point = [[MLNPointFeature alloc] init];
  point.coordinate = CLLocationCoordinate2DMake(0, 0);
  MLNShapeSource *source = [[MLNShapeSource alloc] initWithIdentifier:@"points"
                                                             features:@[ point ]
                                                              options:nil];
  MLNCircleStyleLayer *layer = [[MLNCircleStyleLayer alloc] initWithIdentifier:@"circles"
                                                                        source:source];
  [style addSource:source];
  [style addLayer:layer];

  UIGraphicsImageRendererFormat *format = [[UIGraphicsImageRendererFormat alloc] init];
  format.scale = 1;
  UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(2, 2)
                                                                             format:format];
  UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
    [[UIColor redColor] setFill];
    [context fillRect:CGRectMake(0, 0, 2, 2)];
  }];
  [style setImage:image forName:@"marker"];
  NSData *imageData = UIImagePNGRepresentation([style imageForName:@"marker"]);
  XCTAssertNotNil(imageData);

  style.projection = [NSExpression expressionForConstantValue:@"globe"];
  style.projection = [NSExpression expressionForConstantValue:@"mercator"];

  XCTAssertEqual(self.mapView.style, style);
  XCTAssertEqual([style sourceWithIdentifier:@"points"], source);
  XCTAssertEqual([style layerWithIdentifier:@"circles"], layer);
  XCTAssertEqualObjects(UIImagePNGRepresentation([style imageForName:@"marker"]), imageData);
}

- (void)testGlobeSuppliesUnitSphereGeometryToBothMetalCallbacks {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];

  [self assertRenderedTransition:1];
}

- (void)testGlobePresetBlendsAtIntermediateZoom {
  self.mapView.zoomLevel = 11.5;
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];

  [self assertRenderedTransition:0.5];
}

- (void)testGlobePresetUsesMercatorAtHighZoom {
  self.mapView.zoomLevel = 12;
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];

  [self assertRenderedTransition:0];
}

- (void)testZoomExpressionSelectsGlobeBelowItsStop {
  self.mapView.style.projection = [self zoomProjectionExpression];

  [self assertRenderedTransition:1];
}

- (void)testZoomExpressionSelectsMercatorAtItsStop {
  self.mapView.zoomLevel = 5;
  self.mapView.style.projection = [self zoomProjectionExpression];

  [self assertRenderedTransition:0];
}

- (void)testClearingProjectionRestoresMercatorRendering {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];
  [self assertRenderedTransition:1];

  self.mapView.style.projection = nil;

  [self assertRenderedTransition:0];
}

- (void)testVisibleGlobeBoundsContainVisibleLatitudeExtrema {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];
  [self assertRenderedTransition:1];
  self.mapView.zoomLevel = 0;

  for (NSNumber *bearing in @[ @0, @22.5 ]) {
    self.mapView.direction = bearing.doubleValue;
    XCTAssertEqualWithAccuracy(self.mapView.direction, bearing.doubleValue, 0.0001);
    const MLNCoordinateBounds bounds = self.mapView.visibleCoordinateBounds;
    for (NSNumber *latitude in @[ @-75, @-60, @60, @75 ]) {
      const CLLocationCoordinate2D coordinate = CLLocationCoordinate2DMake(latitude.doubleValue, 0);
      const CGPoint point = [self.mapView convertCoordinate:coordinate toPointToView:self.mapView];

      XCTAssertTrue(CGRectContainsPoint(self.mapView.bounds, point));
      XCTAssertFalse(
          self.mapView.mbglMap.isLocationOccluded({coordinate.latitude, coordinate.longitude}));
      XCTAssertTrue(MLNCoordinateInCoordinateBounds(coordinate, bounds),
                    @"Visible latitude %@ at bearing %@ is missing from %@", latitude, bearing,
                    MLNStringFromCoordinateBounds(bounds));
    }
  }
}

- (void)testVisiblePoleBoundsCoverEveryLongitude {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];
  [self assertRenderedTransition:1];

  for (NSNumber *latitude in @[ @-80, @80 ]) {
    self.mapView.centerCoordinate = CLLocationCoordinate2DMake(latitude.doubleValue, 0);
    self.mapView.zoomLevel = 0;
    const double poleLatitude = latitude.doubleValue < 0 ? -90.0 : 90.0;
    const CLLocationCoordinate2D pole = CLLocationCoordinate2DMake(poleLatitude, 0);
    const CGPoint point = [self.mapView convertCoordinate:pole toPointToView:self.mapView];

    XCTAssertTrue(CGRectContainsPoint(self.mapView.bounds, point));
    XCTAssertFalse(self.mapView.mbglMap.isLocationOccluded({poleLatitude, 0}));
    const MLNCoordinateBounds bounds = self.mapView.visibleCoordinateBounds;
    XCTAssertTrue(MLNCoordinateInCoordinateBounds(pole, bounds));
    XCTAssertEqualWithAccuracy(bounds.ne.longitude - bounds.sw.longitude, 360.0, 0.0001);
  }
}

- (void)testVisibleGlobeBoundsUnwrapTheAntimeridian {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];
  [self assertRenderedTransition:1];
  self.mapView.centerCoordinate = CLLocationCoordinate2DMake(0, 175);
  self.mapView.zoomLevel = 0;

  const CLLocationCoordinate2D coordinate = CLLocationCoordinate2DMake(0, -175);
  const CGPoint point = [self.mapView convertCoordinate:coordinate toPointToView:self.mapView];
  XCTAssertTrue(CGRectContainsPoint(self.mapView.bounds, point));
  XCTAssertFalse(self.mapView.mbglMap.isLocationOccluded({0, -175}));
  const MLNCoordinateBounds bounds = self.mapView.visibleCoordinateBounds;
  XCTAssertLessThan(bounds.sw.longitude, 175);
  XCTAssertGreaterThan(bounds.ne.longitude, 180);
  XCTAssertTrue(MLNCoordinateInCoordinateBounds(CLLocationCoordinate2DMake(0, 185), bounds));
  XCTAssertLessThan(bounds.ne.longitude - bounds.sw.longitude, 360);
}

- (NSExpression *)zoomProjectionExpression {
  return [NSExpression
      mgl_expressionForSteppingExpression:[NSExpression expressionForVariable:@"zoomLevel"]
                           fromExpression:[NSExpression
                                              expressionForConstantValue:@"vertical-perspective"]
                                    stops:[NSExpression
                                              expressionForConstantValue:@{@5 : @"mercator"}]];
}

- (void)testSetCenterCrossesTheGlobeHandoffWithoutChangingTheRequestedCamera {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];
  [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(-33.8688, 151.2093)
                          zoomLevel:13
                           animated:NO];
  [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(60, 10) zoomLevel:-0.8 animated:NO];
  [self assertCenter:CLLocationCoordinate2DMake(60, 10) zoom:-0.8];
  [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(-33.8688, 151.2093)
                          zoomLevel:13
                           animated:NO];
  [self assertCenter:CLLocationCoordinate2DMake(-33.8688, 151.2093) zoom:13];
}

- (void)testAnimatedCameraEaseCrossesTheGlobeHandoff {
  [self assertCameraTransitionsWithFlight:NO duration:0.05];
}

- (void)testCameraGetSetPreservesGlobeZoomAndPolarLatitude {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];
  self.mapView.mbglMap.setConstrainMode(mln::ConstrainMode::None);
  for (NSNumber *latitude in @[ @60, @89 ]) {
    const CLLocationCoordinate2D center = CLLocationCoordinate2DMake(latitude.doubleValue, 10);
    const double zoom = latitude.doubleValue == 60 ? -0.8 : -5;
    MLNMapCamera *initialCamera = [MLNMapCamera
        cameraLookingAtCenterCoordinate:center
                               altitude:MLNAltitudeForZoomLevel(zoom, 30, center.latitude,
                                                                self.mapView.bounds.size)
                                  pitch:30
                                heading:0];
    [self.mapView setCamera:initialCamera animated:NO];
    [self assertCenter:center zoom:zoom];
    MLNMapCamera *camera = self.mapView.camera;
    [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(-33.8688, 151.2093)
                            zoomLevel:13
                             animated:NO];

    [self.mapView setCamera:camera animated:NO];

    [self assertCenter:center zoom:zoom];
    XCTAssertEqualWithAccuracy(self.mapView.camera.pitch, 30, 0.000001);
  }
}

- (void)testImmediateCameraFlightCrossesTheGlobeHandoff {
  [self assertCameraTransitionsWithFlight:YES duration:0];
}

- (void)testAnimatedCameraFlightCrossesTheGlobeHandoff {
  [self assertCameraTransitionsWithFlight:YES duration:0.05];
}

- (void)assertCameraTransitionsWithFlight:(BOOL)flight duration:(NSTimeInterval)duration {
  self.mapView.style.projection = [NSExpression expressionForConstantValue:@"globe"];
  [self.mapView setCenterCoordinate:CLLocationCoordinate2DMake(-33.8688, 151.2093)
                          zoomLevel:13
                           animated:NO];

  for (NSNumber *reverse in @[ @NO, @YES ]) {
    const CLLocationCoordinate2D center = reverse.boolValue
                                              ? CLLocationCoordinate2DMake(-33.8688, 151.2093)
                                              : CLLocationCoordinate2DMake(60, 10);
    const double zoom = reverse.boolValue ? 13 : -0.8;
    MLNMapCamera *camera = [MLNMapCamera
        cameraLookingAtCenterCoordinate:center
                               altitude:MLNAltitudeForZoomLevel(zoom, 0, center.latitude,
                                                                self.mapView.bounds.size)
                                  pitch:0
                                heading:0];
    XCTestExpectation *finished = [self expectationWithDescription:@"Camera transition completed"];
    if (flight) {
      [self.mapView flyToCamera:camera
                   withDuration:duration
              completionHandler:^{
                [finished fulfill];
              }];
    } else {
      [self.mapView setCamera:camera
                     withDuration:duration
          animationTimingFunction:[CAMediaTimingFunction
                                      functionWithName:kCAMediaTimingFunctionLinear]
                completionHandler:^{
                  [finished fulfill];
                }];
    }
    [self waitForExpectations:@[ finished ] timeout:10];
    [self assertCenter:center zoom:zoom];
  }
}

- (void)assertCenter:(CLLocationCoordinate2D)center zoom:(double)zoom {
  XCTAssertEqualWithAccuracy(self.mapView.centerCoordinate.latitude, center.latitude, 0.000001);
  XCTAssertEqualWithAccuracy(self.mapView.centerCoordinate.longitude, center.longitude, 0.000001);
  XCTAssertEqualWithAccuracy(self.mapView.zoomLevel, zoom, 0.000001);
}

- (void)assertRenderedTransition:(double)transition {
  XCTestExpectation *preDraw = [self expectationWithDescription:@"Metal preDraw context"];
  XCTestExpectation *draw = [self expectationWithDescription:@"Metal draw context"];
  MLNGlobeDrawingProbe *probe = [[MLNGlobeDrawingProbe alloc] initWithIdentifier:@"globe-probe"];
  __weak MLNGlobeDrawingProbe *weakProbe = probe;
  __block BOOL receivedPreDraw = NO;
  __block BOOL receivedDraw = NO;

  probe.preDrawHandler = ^(MLNStyleLayerDrawingContext context) {
    if (receivedPreDraw || std::abs(context.projectionTransition - transition) > 0.0001) {
      return;
    }
    receivedPreDraw = YES;
    XCTAssertNotNil(weakProbe.commandBuffer);
    XCTAssertNotNil(weakProbe.renderPassDesc);
    [self assertDrawingContext:context transition:transition];
    [preDraw fulfill];
  };
  probe.drawHandler = ^(MLNStyleLayerDrawingContext context) {
    if (receivedDraw || std::abs(context.projectionTransition - transition) > 0.0001) {
      return;
    }
    receivedDraw = YES;
    XCTAssertNotNil(weakProbe.commandBuffer);
    XCTAssertNotNil(weakProbe.renderEncoder);
    [self assertDrawingContext:context transition:transition];
    [draw fulfill];
  };
  [self.mapView.style addLayer:probe];

  [self waitForExpectations:@[ preDraw, draw ] timeout:10];

  probe.preDrawHandler = nil;
  probe.drawHandler = nil;
  [self.mapView.style removeLayer:probe];
}

- (void)assertDrawingContext:(MLNStyleLayerDrawingContext)context transition:(double)transition {
  XCTAssertEqual(context.globe, transition > 0);
  XCTAssertEqualWithAccuracy(context.projectionTransition, transition, 0.0001);
  XCTAssertEqualWithAccuracy(context.size.width, 256, 0.001);
  XCTAssertEqualWithAccuracy(context.size.height, 256, 0.001);

  if (transition > 0) {
    MLNMatrix4 matrix = context.globeProjectionMatrix;
    XCTAssertGreaterThan(matrix.m23 + matrix.m33, 0);
    XCTAssertEqualWithAccuracy(matrix.m20 + matrix.m30, 0, 0.0001);
    XCTAssertEqualWithAccuracy(matrix.m21 + matrix.m31, 0, 0.0001);
    XCTAssertEqualWithAccuracy(context.globeClippingPlane[0], 0, 0.0001);
    XCTAssertEqualWithAccuracy(context.globeClippingPlane[1], 0, 0.0001);
    XCTAssertEqualWithAccuracy(context.globeClippingPlane[2], 1, 0.0001);
    XCTAssertGreaterThan(context.globeClippingPlane[3], -1);
    XCTAssertLessThan(context.globeClippingPlane[3], 0);
  }
}

@end

#endif
