import { useCallback, useEffect, useRef, useState } from 'react';

/** Elapsed milliseconds while `playing`, frozen on pause, and resettable via the returned callback. */
export function usePlaybackClock(playing: boolean): [number, () => void] {
  const [elapsed, setElapsed] = useState(0);
  const frameRef = useRef<number>();
  const lastTimeRef = useRef<number | null>(null);

  useEffect(() => {
    if (!playing) {
      lastTimeRef.current = null;
      return;
    }

    const tick = (now: number) => {
      if (lastTimeRef.current !== null) {
        setElapsed((prev) => prev + (now - lastTimeRef.current!));
      }
      lastTimeRef.current = now;
      frameRef.current = requestAnimationFrame(tick);
    };
    frameRef.current = requestAnimationFrame(tick);

    return () => {
      if (frameRef.current !== undefined) cancelAnimationFrame(frameRef.current);
      lastTimeRef.current = null;
    };
  }, [playing]);

  const reset = useCallback(() => setElapsed(0), []);

  return [elapsed, reset];
}
