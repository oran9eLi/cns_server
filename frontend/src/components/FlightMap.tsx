import AMapLoader from "@amap/amap-jsapi-loader";
import { LocateFixed } from "lucide-react";
import { useEffect, useRef, useState } from "react";

import type { FlightPositionView } from "../utils/flightDashboard.js";

declare global {
  interface Window {
    _AMapSecurityConfig?: {
      securityJsCode?: string;
      serviceHost?: string;
    };
  }
}

type LngLatTuple = [number, number];
type MapStatus = "loading" | "ready" | "error";
type PositionStatus = "waiting" | "backend" | "verified" | "fallback" | "error";
type LngLatObject = {
  getLng: () => number;
  getLat: () => number;
};
type ConvertResult = {
  info?: string;
  locations?: Array<LngLatObject | LngLatTuple>;
};
type FlightMapInstance = {
  add: (overlays: unknown | unknown[]) => void;
  addControl: (control: unknown) => void;
  destroy: () => void;
  setCenter: (position: LngLatTuple) => void;
  setZoomAndCenter: (zoom: number, position: LngLatTuple) => void;
};
type FlightMarkerInstance = {
  setAngle: (angle: number) => void;
  setPosition: (position: LngLatTuple) => void;
};
type FlightPolylineInstance = {
  setPath: (path: LngLatTuple[]) => void;
};
type FlightAMapNamespace = {
  Map: new (container: HTMLElement, options: Record<string, unknown>) => FlightMapInstance;
  Marker: new (options: Record<string, unknown>) => FlightMarkerInstance;
  Polyline: new (options: Record<string, unknown>) => FlightPolylineInstance;
  Scale: new () => unknown;
  ToolBar: new (options: Record<string, unknown>) => unknown;
  convertFrom: (
    position: LngLatTuple,
    sourceType: "gps",
    callback: (status: string, result: ConvertResult) => void
  ) => void;
};

const API_VALIDATION_INTERVAL_MS = 60_000;
const MAX_TRACK_POINTS = 500;
const MAX_VALIDATION_DIFFERENCE_METERS = 10;

export function FlightMap({ position }: { position: FlightPositionView }) {
  const containerRef = useRef<HTMLDivElement>(null);
  const amapRef = useRef<FlightAMapNamespace | null>(null);
  const mapRef = useRef<FlightMapInstance | null>(null);
  const markerRef = useRef<FlightMarkerInstance | null>(null);
  const polylineRef = useRef<FlightPolylineInstance | null>(null);
  const trackRef = useRef<LngLatTuple[]>([]);
  const latestPositionRef = useRef<LngLatTuple | null>(null);
  const centeredRef = useRef(false);
  const conversionSequenceRef = useRef(0);
  const lastValidationAtRef = useRef(0);
  const [status, setStatus] = useState<MapStatus>("loading");
  const [positionStatus, setPositionStatus] = useState<PositionStatus>("waiting");
  const [errorMessage, setErrorMessage] = useState("");

  useEffect(() => {
    const key = import.meta.env.VITE_AMAP_KEY?.trim();
    const securityJsCode = import.meta.env.VITE_AMAP_SECURITY_CODE?.trim();

    if (!key || !securityJsCode) {
      setStatus("error");
      setErrorMessage("请在 frontend/.env.local 中配置高德 Key 和 securityJsCode");
      return;
    }

    let cancelled = false;
    window._AMapSecurityConfig = { securityJsCode };

    AMapLoader.load({
      key,
      version: "2.0",
      plugins: ["AMap.Scale", "AMap.ToolBar"]
    })
      .then((loadedAMap) => {
        if (cancelled || !containerRef.current) return;
        const AMap = loadedAMap as FlightAMapNamespace;
        const map = new AMap.Map(containerRef.current, {
          viewMode: "2D",
          zoom: 11,
          resizeEnable: true,
          mapStyle: "amap://styles/whitesmoke"
        });
        map.addControl(new AMap.Scale());
        map.addControl(new AMap.ToolBar({ position: "RB" }));
        amapRef.current = AMap;
        mapRef.current = map;
        setStatus("ready");
      })
      .catch((error: unknown) => {
        if (cancelled) return;
        setStatus("error");
        setErrorMessage(error instanceof Error ? error.message : "高德地图加载失败");
      });

    return () => {
      cancelled = true;
      conversionSequenceRef.current += 1;
      mapRef.current?.destroy();
      amapRef.current = null;
      mapRef.current = null;
      markerRef.current = null;
      polylineRef.current = null;
      trackRef.current = [];
      latestPositionRef.current = null;
      centeredRef.current = false;
    };
  }, []);

  useEffect(() => {
    const AMap = amapRef.current;
    if (status !== "ready" || !AMap || !mapRef.current) return;

    if (position.fixValid === false) {
      conversionSequenceRef.current += 1;
      setPositionStatus("waiting");
      return;
    }

    const gcj02 = coordinateTuple(position.longitudeGcj02, position.latitudeGcj02);
    const wgs84 = coordinateTuple(position.longitudeWgs84, position.latitudeWgs84);

    if (gcj02) {
      updateMapPosition(AMap, gcj02, position.heading);
      setPositionStatus("backend");

      if (wgs84 && Date.now() - lastValidationAtRef.current >= API_VALIDATION_INTERVAL_MS) {
        lastValidationAtRef.current = Date.now();
        convertWithAmap(AMap, wgs84, (officialPosition) => {
          const difference = distanceMeters(gcj02, officialPosition);
          if (difference > MAX_VALIDATION_DIFFERENCE_METERS) {
            updateMapPosition(AMap, officialPosition, position.heading);
            setPositionStatus("fallback");
            return;
          }
          setPositionStatus("verified");
        }, () => {
          setPositionStatus("backend");
        });
      }
      return;
    }

    if (wgs84) {
      convertWithAmap(AMap, wgs84, (officialPosition) => {
        updateMapPosition(AMap, officialPosition, position.heading);
        setPositionStatus("fallback");
      }, () => {
        setPositionStatus("error");
      });
      return;
    }

    setPositionStatus("waiting");
  }, [
    position.fixValid,
    position.heading,
    position.latitudeGcj02,
    position.latitudeWgs84,
    position.longitudeGcj02,
    position.longitudeWgs84,
    status
  ]);

  const convertWithAmap = (
    AMap: FlightAMapNamespace,
    wgs84: LngLatTuple,
    onSuccess: (position: LngLatTuple) => void,
    onFailure: () => void
  ) => {
    const sequence = ++conversionSequenceRef.current;
    AMap.convertFrom(wgs84, "gps", (convertStatus, result) => {
      if (sequence !== conversionSequenceRef.current) return;
      const converted = convertStatus === "complete" && result.info === "ok"
        ? lngLatTuple(result.locations?.[0])
        : null;
      if (!converted) {
        onFailure();
        return;
      }
      onSuccess(converted);
    });
  };

  const updateMapPosition = (
    AMap: FlightAMapNamespace,
    nextPosition: LngLatTuple,
    heading: number | null
  ) => {
    const map = mapRef.current;
    if (!map) return;
    latestPositionRef.current = nextPosition;

    if (!polylineRef.current) {
      polylineRef.current = new AMap.Polyline({
        path: [nextPosition],
        strokeColor: "#1769e0",
        strokeWeight: 4,
        strokeOpacity: 0.86,
        lineJoin: "round",
        lineCap: "round",
        zIndex: 40
      });
      map.add(polylineRef.current);
    }

    if (!markerRef.current) {
      markerRef.current = new AMap.Marker({
        position: nextPosition,
        anchor: "center",
        angle: normalizeHeading(heading),
        content: `
          <div class="flight-map-aircraft-marker" aria-label="飞行器实时位置">
            <svg viewBox="0 0 48 48" aria-hidden="true">
              <g class="drone-arms">
                <path d="M15 15 33 33M33 15 15 33" />
              </g>
              <g class="drone-rotors">
                <circle cx="12" cy="12" r="7" />
                <circle cx="36" cy="12" r="7" />
                <circle cx="12" cy="36" r="7" />
                <circle cx="36" cy="36" r="7" />
              </g>
              <path class="drone-body" d="M19 16h10l3 18-8-4-8 4 3-18Z" />
              <path class="drone-nose" d="m24 7 4 8h-8l4-8Z" />
              <circle class="drone-core" cx="24" cy="23" r="3" />
            </svg>
          </div>`,
        title: "飞行器实时位置",
        zIndex: 120
      });
      map.add(markerRef.current);
    } else {
      markerRef.current.setPosition(nextPosition);
      markerRef.current.setAngle(normalizeHeading(heading));
    }

    appendTrackPoint(nextPosition);
    polylineRef.current.setPath(trackRef.current);
    if (!centeredRef.current) {
      map.setZoomAndCenter(16, nextPosition);
      centeredRef.current = true;
    }
  };

  const appendTrackPoint = (nextPosition: LngLatTuple) => {
    const previous = trackRef.current.at(-1);
    if (previous && previous[0] === nextPosition[0] && previous[1] === nextPosition[1]) return;
    trackRef.current = [...trackRef.current, nextPosition].slice(-MAX_TRACK_POINTS);
  };

  const locateAircraft = () => {
    const map = mapRef.current;
    const latestPosition = latestPositionRef.current;
    if (!map || !latestPosition) {
      setPositionStatus("waiting");
      return;
    }
    map.setZoomAndCenter(17, latestPosition);
  };

  return (
    <div className="flight-map-stage" aria-label="实时飞行地图">
      <div className="flight-map-canvas" ref={containerRef} />
      {status !== "ready" && (
        <div className={`flight-map-status is-${status}`} role={status === "error" ? "alert" : "status"}>
          {status === "loading" ? "正在加载高德地图…" : errorMessage}
        </div>
      )}
      {status === "ready" && (
        <>
          <div className={`flight-map-location-status is-${positionStatus}`}>
            {positionStatusLabel(positionStatus)}
          </div>
          <button
            className="flight-map-locate-button"
            type="button"
            onClick={locateAircraft}
            disabled={!latestPositionRef.current}
            aria-label="定位到飞机位置"
            title={latestPositionRef.current ? "定位到飞机位置" : "等待飞机定位"}
          >
            <LocateFixed aria-hidden="true" size={18} strokeWidth={2.2} />
          </button>
        </>
      )}
    </div>
  );
}

function coordinateTuple(longitude: number | null, latitude: number | null): LngLatTuple | null {
  if (longitude === null || latitude === null) return null;
  if (!Number.isFinite(longitude) || !Number.isFinite(latitude)) return null;
  if (longitude < -180 || longitude > 180 || latitude < -90 || latitude > 90) return null;
  if (longitude === 0 && latitude === 0) return null;
  return [longitude, latitude];
}

function lngLatTuple(value: LngLatObject | LngLatTuple | undefined): LngLatTuple | null {
  if (!value) return null;
  if (Array.isArray(value)) return coordinateTuple(value[0], value[1]);
  return coordinateTuple(value.getLng(), value.getLat());
}

function normalizeHeading(value: number | null): number {
  if (value === null || !Number.isFinite(value)) return 0;
  return ((value % 360) + 360) % 360;
}

function distanceMeters(first: LngLatTuple, second: LngLatTuple): number {
  const earthRadius = 6_371_000;
  const latitude1 = first[1] * Math.PI / 180;
  const latitude2 = second[1] * Math.PI / 180;
  const latitudeDelta = (second[1] - first[1]) * Math.PI / 180;
  const longitudeDelta = (second[0] - first[0]) * Math.PI / 180;
  const a = Math.sin(latitudeDelta / 2) ** 2
    + Math.cos(latitude1) * Math.cos(latitude2) * Math.sin(longitudeDelta / 2) ** 2;
  return earthRadius * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function positionStatusLabel(status: PositionStatus): string {
  return ["backend", "verified", "fallback"].includes(status)
    ? "实时定位"
    : "等待定位";
}
